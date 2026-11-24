# Host unit tests: run on your computer, no board needed.
#   make test          stand-in FFT (needs only gcc)
#   make test-cmsis    the REAL CMSIS-DSP FFT; pass the path of the module:
#                      make test-cmsis CMSIS_DSP=/path/to/modules/lib/cmsis-dsp
CC ?= gcc
SRC = ../../src
CMSIS_DSP ?= ../../../modules/lib/cmsis-dsp

test: test_main
	./test_main
test_main: test_main.c fft_host.c $(SRC)/cycle.c $(SRC)/dsp.c $(SRC)/cfg.c
	$(CC) -O1 -g -Wall -Wextra -I$(SRC) -o $@ $^ -lm

CMS = $(CMSIS_DSP)/Source/TransformFunctions
TAB = $(CMSIS_DSP)/Source/CommonTables
CMSIS_SRCS = $(CMS)/arm_rfft_fast_f32.c $(CMS)/arm_rfft_fast_init_f32.c $(CMS)/arm_cfft_f32.c \
	$(CMS)/arm_cfft_init_f32.c $(CMS)/arm_cfft_radix8_f32.c $(CMS)/arm_bitreversal2.c \
	$(CMS)/arm_bitreversal.c $(TAB)/arm_common_tables.c $(TAB)/arm_const_structs.c

test-cmsis: test_cmsis
	./test_cmsis
test_cmsis: test_main.c fft_cmsis_host.c $(SRC)/cycle.c $(SRC)/dsp.c $(SRC)/cfg.c
	$(CC) -O1 -g -w -Istub -I$(CMSIS_DSP)/Include -I$(CMSIS_DSP)/PrivateInclude -I$(SRC) -o $@ $^ $(CMSIS_SRCS) -lm

clean:
	rm -rf test_main test_cmsis obj
