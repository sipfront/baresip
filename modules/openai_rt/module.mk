MOD	:= openai_rt
$(MOD)_SRCS	+= openai_rt.c
$(MOD)_LFLAGS	+= -lcurl -lwebsockets