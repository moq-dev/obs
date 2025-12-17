/*
 * C-compatible logging header for obs-moq plugin
 * Copyright (C) 2024 OBS Plugin Template
 */

#pragma once

#include <obs-module.h>

#define CLOG(level, format, ...) blog(level, "[obs-moq] " format, ##__VA_ARGS__)
#define CLOG_DEBUG(format, ...) CLOG(LOG_DEBUG, format, ##__VA_ARGS__)
#define CLOG_INFO(format, ...) CLOG(LOG_INFO, format, ##__VA_ARGS__)
#define CLOG_WARNING(format, ...) CLOG(LOG_WARNING, format, ##__VA_ARGS__)
#define CLOG_ERROR(format, ...) CLOG(LOG_ERROR, format, ##__VA_ARGS__)
