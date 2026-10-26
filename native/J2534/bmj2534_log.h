// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 BUSMUST Tech Co.,Ltd.
#pragma once
#include <memory>

namespace bmj { namespace diagnostic {
struct Sink;
using Session = std::shared_ptr<Sink>;
// A session is retained by live devices/API attempts. Its last owner requests
// an asynchronous drain; no file work or thread join occurs on the caller.
Session start() noexcept;
void event(const char* stage,unsigned long a=0,unsigned long b=0,
           unsigned long c=0,const char* detail=nullptr) noexcept;
void event(const Session& session,const char* stage,unsigned long a=0,
           unsigned long b=0,unsigned long c=0,const char* detail=nullptr) noexcept;
void error(long status,const char* detail) noexcept;
class ApiScope {
    const char* previous_;
    unsigned long previousTarget_;
public:
    ApiScope(const char* name,unsigned long target=0) noexcept;
    ~ApiScope() noexcept;
};
}}
