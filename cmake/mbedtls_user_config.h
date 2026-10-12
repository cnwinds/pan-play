#pragma once

// 预读会同时开多条 HTTPS 连接，PSA 和随机数生成器要加锁。
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_PTHREAD
