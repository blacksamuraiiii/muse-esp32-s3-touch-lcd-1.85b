/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "freertos/FreeRTOS.h"

/* The harness's test queue: a plain array behind a void pointer, as in the
 * production code's QueueHandle_t. The harness defines the operations. */
typedef void *QueueHandle_t;

BaseType_t xQueueSend(QueueHandle_t q, const void *p, TickType_t t);
BaseType_t xQueueSendToFront(QueueHandle_t q, const void *p, TickType_t t);
BaseType_t xQueuePeek(QueueHandle_t q, void *p, TickType_t t);
BaseType_t xQueueReceive(QueueHandle_t q, void *p, TickType_t t);
