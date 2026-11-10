/*
 * cubecore.h —— EspCube 公共基础设施总头文件
 *
 * 设计原则:
 *   1. 本层不含任何业务逻辑, 只提供跨模块复用的基础能力;
 *   2. 跨模块通信一律走事件总线(cube_event_bus), 禁止模块间直接相互引用,
 *      以保证《02_详细设计.md》§2 的分层依赖原则。
 */
#pragma once

#include "cube_event_bus.h"
#include "cube_ringbuf.h"
#include "cube_resampler.h"
#include "cube_fft.h"
#include "cube_tasks.h"
