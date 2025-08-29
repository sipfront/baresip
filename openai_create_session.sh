#!/bin/sh

TOKEN=$1
PROMPT=$2
if [ -z "$TOKEN" ] || [ -z "$PROMPT" ]; then
    echo "Usage: $0 <api-key> <sys-prompt>"
    exit 1
fi

curl -X POST https://api.openai.com/v1/realtime/client_secrets \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{
    "expires_after": { "anchor": "created_at", "seconds": 3600 },
    "session": {
      "type": "realtime",
      "model": "gpt-realtime",
      "tool_choice": "none",
      "instructions": "'"$PROMPT"'",
      "output_modalities": ["audio"],
      "audio": {
        "input": {
          "format": {
            "type": "audio/pcm",
            "rate": 24000
          },
          "noise_reduction": { "type": "near_field" },
          "turn_detection": {
            "type":"server_vad",
            "threshold":0.5,
            "prefix_padding_ms":300,
            "silence_duration_ms":500,
            "create_response": true,
            "interrupt_response":true
          }
        },
        "output": {
          "format": {
            "type": "audio/pcm",
            "rate": 24000
          },
          "voice": "sage"
        }
      }
    }
  }'
