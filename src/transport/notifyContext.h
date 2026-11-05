#pragma once
#include <functional>

struct NotifyContext
{
    // put other states here
    // rn just this callback
    std::function<void()> notifySubmission;
};