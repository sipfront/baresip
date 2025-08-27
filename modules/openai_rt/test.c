/**
 * @file test.c  Minimal harness to exercise websocket + audio append
 *
 * Usage:
 *   ./openai_rt_test /path/to/24k_mono_pcm16.wav
 *
 * Notes:
 * - Requires your dump.c to have dump_audio_response() that writes to /tmp/oai_response.wav
 * - Chunks are >= 100ms (here: 200ms = 9600 bytes) before each commit
 * - Session update is already sent in websocket.c on connect
 */

 #include <re.h>
 #include <rem.h>
 #include <baresip.h>
 #include <pthread.h>
 #include <json-c/json.h>
 #include <sndfile.h>
 #include <stdio.h>
 #include <stdlib.h>
 #include <string.h>
 #include "openai_rt.h"
 
 /* ========= EDIT ME ========= */
 #define OAI_API_KEY "sk-proj-YOUR_KEY_HERE"
 /* =========================== */

 char json_msg[65536];
 
 static int send_append_chunk(const uint8_t *bytes, size_t nbytes) {
     info("test: in send_append_chunk\n");
     char *b64 = encode_audio_base64(bytes, nbytes);
     if (!b64) {
         warning("test: base64 encode failed for %zu bytes\n", nbytes);
         info("test: failed to encode audio\n");
         return EINVAL;
     }
     
     snprintf(json_msg, sizeof(json_msg),
         "{"
           "\"type\":\"input_audio_buffer.append\","
           "\"audio\":\"%s\""
         "}",
         b64
     );
     mem_deref(b64);

     info("test: sending append_chunk: %s\n", json_msg);
 
     int err = queue_message_to_openai(json_msg, str_len(json_msg), NULL, NULL);
     return err;
 }
 
 static int send_commit(void) {
    if (!g_oairt.session_cfg_applied) {
        info("test: skip append/commit until session.updated\n");
        return 0; /* or EAGAIN if you prefer */
    }
     static const char *msg = "{\"type\":\"input_audio_buffer.commit\"}";
     return queue_message_to_openai(msg, str_len(msg), NULL, NULL);
 }
 
 static int send_response_create(void) {
     /* Ask for both audio + text so we can also see transcripts coming back */
     snprintf(json_msg, sizeof(json_msg),
         "{"
           "\"type\":\"response.create\","
           "\"response\":{"
              "\"modalities\":[\"audio\",\"text\"],"
              "\"instructions\":\"%s\""
           "}"
         "}",
         str_isset(g_oairt.prompt) ? g_oairt.prompt :
         "You are a helpful voice assistant for phone calls."
     );
     int err = queue_message_to_openai(json_msg, str_len(json_msg), NULL, NULL);
     return err;
 }
 
 int main(int argc, char **argv)
 {
     int err = 0;
 
     if (argc != 2) {
         re_fprintf(stderr, "Usage: %s /path/to/24k_mono_pcm16.wav\n", argv[0]);
         return 2;
     }
     const char *wav_path = argv[1];
 
     /* ===== Minimal g_oairt bootstrapping ===== */
     memset(&g_oairt, 0, sizeof(g_oairt));
     str_ncpy(g_oairt.api_key, OAI_API_KEY, sizeof(g_oairt.api_key));
     str_ncpy(g_oairt.prompt, "You are a helpful voice assistant for phone calls.",
              sizeof(g_oairt.prompt));
 
     if (!str_isset(g_oairt.api_key)) {
         warning("test: API key missing (edit OAI_API_KEY)\n");
         return 1;
     }
 
     /* Init dumpers first so we capture our input wav into /tmp/sip_to_oai.wav */
     if ((err = dump_init())) {
         warning("test: dump_init failed: %m\n", err);
         /* keep going; not fatal */
         err = 0;
     }
 
     /* Start the websocket subsystem (spins its own thread, auto-connects) */
     if ((err = websocket_init())) {
         warning("test: websocket_init failed: %m\n", err);
         goto out;
     }
 
     /* Wait until socket is usable */
     if ((err = websocket_wait_ready(10000))) {  /* 10s */
         warning("test: websocket not ready: %m\n", err);
         goto out;
     }
 
     info("test: websocket ready, sending audio…\n");
 
     /* ===== Load WAV (expect 24 kHz mono PCM16) ===== */
     SF_INFO sfi;
     memset(&sfi, 0, sizeof(sfi));
     SNDFILE *sf = sf_open(wav_path, SFM_READ, &sfi);
     if (!sf) {
         warning("test: cannot open wav: %s\n", wav_path);
         err = EINVAL;
         goto out;
     }
 
     if (sfi.channels != 1 || sfi.samplerate != 24000 || (sfi.format & SF_FORMAT_PCM_16) == 0) {
         warning("test: WAV must be 24kHz mono PCM16. Got: %d Hz, ch=%d, fmt=0x%x\n",
                 sfi.samplerate, sfi.channels, sfi.format);
         sf_close(sf);
         err = EINVAL;
         goto out;
     }
 
     /* Read entire file into memory (ok for small tests) */
     sf_count_t total_samples = sfi.frames * sfi.channels;
     size_t total_bytes = (size_t)total_samples * sizeof(int16_t);
     int16_t *pcm = mem_zalloc(total_bytes, NULL);
     if (!pcm) {
         sf_close(sf);
         err = ENOMEM;
         goto out;
     }
     sf_count_t got = sf_read_short(sf, pcm, total_samples);
     sf_close(sf);
     if (got != total_samples) {
         warning("test: short read: wanted %lld samples, got %lld\n",
                 (long long)total_samples, (long long)got);
         /* continue with what we have */
         total_samples = got;
         total_bytes   = (size_t)got * sizeof(int16_t);
     }
 
     /* Dump our input for parity (goes to /tmp/sip_to_oai.wav via dump_audio) */
     dump_audio(pcm, (size_t)total_samples);
 
     /* ===== Ship WAV in chunks ===== */
     const size_t bytes_per_ms = 24000 /*Hz*/ * 2 /*bytes*/ / 1000;
     const size_t chunk_ms     = 100;
     const size_t chunk_bytes  = bytes_per_ms * chunk_ms;  
     const size_t commit_min   = bytes_per_ms * 800;  
 
     size_t sent_in_current_commit = 0;
     const uint8_t *p = (const uint8_t *)pcm;
     size_t left = total_bytes;
     info("test: sending total of %zu bytes\n", total_bytes);
 
     while (left) {
         size_t n = (left < chunk_bytes) ? left : chunk_bytes;
 
         /* append */
         info("test: sending %zu bytes\n", n);
         if ((err = send_append_chunk(p, n))) {
             warning("test: append failed: %m\n", err);
             break;
         }
 
         p += n;
         left -= n;
         sent_in_current_commit += n;
 
         /* commit when we reach >=100ms */
         if (sent_in_current_commit >= commit_min) {
            /*
             info("test: committing %.1f ms (%zu bytes)\n",
                  (double)sent_in_current_commit / 2 / 24000 * 1000.0,
                  sent_in_current_commit);
            */
             if ((err = send_commit())) {
                 warning("test: commit failed: %m\n", err);
                 break;
             }
             /* Start a turn explicitly (easiest for debugging) */
             //if ((err = send_response_create())) {
             //    warning("test: response.create failed: %m\n", err);
             //    break;
             //}
             sent_in_current_commit = 0;
         }
     }
 
     /* If there’s a small tail <100ms, batch it up and still do one last commit+turn */
     if (!err && sent_in_current_commit > 0) {
         info("test: final small commit %.1f ms (%zu bytes)\n",
              (double)sent_in_current_commit / 2 / 24000 * 1000.0,
              sent_in_current_commit);
         err = send_commit();
         if (!err) err = send_response_create();
     }
 
     /* ===== Let responses flow back for a bit ===== */
     info("test: waiting for replies (5s)…\n");
     sys_msleep(10000);
 
     /* Cleanup */
     mem_deref(pcm);
 
 out:
     websocket_close();
     dump_close();
 
     info("test: done (err=%d). Input dump: /tmp/sip_to_oai.wav, Output dump: /tmp/oai_response.wav\n", err);
     return err ? 1 : 0;
 }
