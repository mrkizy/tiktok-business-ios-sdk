//
//  TTSDKCrashReportSinkConsole.m
//
//  Created by Karl Stenerud on 2012-05-11.
//
//  Copyright (c) 2012 Karl Stenerud. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall remain in place
// in this source code.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#import "TTSDKCrashReportSinkConsole.h"
#import "TTSDKCrashReport.h"
#import "TTSDKCrashReportFilterAppleFmt.h"
#import "TTSDKCrashReportFilterBasic.h"
#import "TikTokAppEvent.h"
#import "TikTokBusiness.h"
#import "TikTokBusiness+private.h"

// #define TTSDKLogger_LocalLevel TRACE
#import "TTSDKLogger.h"

@implementation TTSDKCrashReportSinkConsole

- (id<TTSDKCrashReportFilter>)defaultCrashReportFilterSet
{
    return [[TTSDKCrashReportFilterPipeline alloc] initWithFilters:@[
        [[TTSDKCrashReportFilterAppleFmt alloc] initWithReportStyle:TTSDKAppleReportStyleSymbolicated],
        self,
    ]];
}

- (void)filterReports:(NSArray<id<TTSDKCrashReport>> *)reports onCompletion:(TTSDKCrashReportFilterCompletion)onCompletion
{
    int i = 0;
    for (TTSDKCrashReportString *report in reports) {
        if ([report isKindOfClass:[TTSDKCrashReportString class]] == NO) {
            TTSDKLOG_ERROR(@"Unexpected non-string report: %@", report);
            continue;
        }
        printf("Report %d:\n%s\n", ++i, report.value.UTF8String);
    }

    ttsdkcrash_callCompletion(onCompletion, reports, nil);
}

@end
