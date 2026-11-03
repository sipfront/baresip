project(turndetect)

list(APPEND MODULES_DETECTED ${PROJECT_NAME})
set(MODULES_DETECTED ${MODULES_DETECTED} PARENT_SCOPE)

# The WebRTC voice activity detector, vendored from libfvad (BSD, see
# fvad/LICENSE): the webrtc engine of the filter.
set(FVAD_SRCS
  fvad/src/fvad.c
  fvad/src/signal_processing/division_operations.c
  fvad/src/signal_processing/energy.c
  fvad/src/signal_processing/get_scaling_square.c
  fvad/src/signal_processing/resample_48khz.c
  fvad/src/signal_processing/resample_by_2_internal.c
  fvad/src/signal_processing/resample_fractional.c
  fvad/src/signal_processing/spl_inl.c
  fvad/src/vad/vad_core.c
  fvad/src/vad/vad_filterbank.c
  fvad/src/vad/vad_gmm.c
  fvad/src/vad/vad_sp.c
)

# Third-party code, not held to this tree's warnings
set_source_files_properties(${FVAD_SRCS} PROPERTIES COMPILE_OPTIONS "-w")

set(SRCS turndetect.c ${FVAD_SRCS})

if(STATIC)
  add_library(${PROJECT_NAME} OBJECT ${SRCS})
else()
  add_library(${PROJECT_NAME} MODULE ${SRCS})
endif()
