//
//  TTSDKCrashInstallationEmail.m
//
//  Created by Karl Stenerud on 2013-03-02.
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

#import "TTSDKCrashInstallationEmail.h"
#import "TTSDKCrashInstallation+Private.h"
#import "TTSDKCrashReportFilterAlert.h"
#import "TTSDKCrashReportSinkEMail.h"

@interface TTSDKCrashInstallationEmail ()

@property(nonatomic, readwrite, copy) NSDictionary *defaultFilenameFormats;

@end

@implementation TTSDKCrashInstallationEmail

+ (instancetype)sharedInstance
{
    static TTSDKCrashInstallationEmail *sharedInstance = nil;
    static dispatch_once_t onceToken;

    dispatch_once(&onceToken, ^{
        sharedInstance = [[TTSDKCrashInstallationEmail alloc] init];
    });
    return sharedInstance;
}

- (id)init
{
    if ((self = [super
             initWithRequiredProperties:[NSArray arrayWithObjects:@"recipients", @"subject", @"filenameFmt", nil]])) {
        NSString *bundleName = [[[NSBundle mainBundle] infoDictionary] objectForKey:@"CFBundleName"];
        _subject = [NSString stringWithFormat:@"Crash Report (%@)", bundleName];
        _defaultFilenameFormats = [NSDictionary
            dictionaryWithObjectsAndKeys:[NSString stringWithFormat:@"crash-report-%@-%%d.txt.gz", bundleName],
                                         [NSNumber numberWithInt:TTSDKCrashEmailReportStyleApple],
                                         [NSString stringWithFormat:@"crash-report-%@-%%d.json.gz", bundleName],
                                         [NSNumber numberWithInt:TTSDKCrashEmailReportStyleJSON], nil];
        [self setReportStyle:TTSDKCrashEmailReportStyleJSON useDefaultFilenameFormat:YES];
    }
    return self;
}

- (void)setReportStyle:(TTSDKCrashEmailReportStyle)reportStyle useDefaultFilenameFormat:(BOOL)useDefaultFilenameFormat
{
    self.reportStyle = reportStyle;

    if (useDefaultFilenameFormat) {
        self.filenameFmt = [self.defaultFilenameFormats objectForKey:[NSNumber numberWithInt:(int)reportStyle]];
    }
}

- (id<TTSDKCrashReportFilter>)sink
{
    TTSDKCrashReportSinkEMail *sink = [[TTSDKCrashReportSinkEMail alloc] initWithRecipients:self.recipients
                                                                              subject:self.subject
                                                                              message:self.message
                                                                          filenameFmt:self.filenameFmt];

    switch (self.reportStyle) {
        case TTSDKCrashEmailReportStyleApple:
            return [sink defaultCrashReportFilterSetAppleFmt];
        case TTSDKCrashEmailReportStyleJSON:
            return [sink defaultCrashReportFilterSet];
    }
}

@end
