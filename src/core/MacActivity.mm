#include "MacActivity.hpp"

#import <Foundation/Foundation.h>

MacActivity::MacActivity(const std::string& reason)
{
    @autoreleasepool {
        NSString* ns_reason =
            [[NSString alloc] initWithUTF8String:reason.c_str()];
        NSActivityOptions options =
            NSActivityUserInitiated |
            NSActivityLatencyCritical |
            NSActivityIdleSystemSleepDisabled |
            NSActivitySuddenTerminationDisabled |
            NSActivityAutomaticTerminationDisabled;
        id token = [[NSProcessInfo processInfo]
            beginActivityWithOptions:options
            reason:ns_reason ?: @"tmplay active work"];
        token_ = (__bridge_retained void*)token;
    }
}

MacActivity::~MacActivity()
{
    if (token_ == nullptr) {
        return;
    }
    @autoreleasepool {
        id token = (__bridge_transfer id)token_;
        [[NSProcessInfo processInfo] endActivity:token];
        token_ = nullptr;
    }
}
