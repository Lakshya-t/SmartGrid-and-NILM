cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(grid_monitor)

target_sources(app PRIVATE
	src/main.c
	src/cfg.c
	src/cycle.c
	src/dsp.c
	src/fft_port.c
	src/sampler.c
	src/protect.c
	src/dsp_task.c
	src/logstore.c
	src/telemetry.c
	src/grid_shell.c
)
target_include_directories(app PRIVATE src)
