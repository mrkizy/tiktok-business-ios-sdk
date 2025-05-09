//
//  TTSDKCrashReportSinkEMail.m
//
//  Created by Karl Stenerud on 2012-05-06.
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

#import "TTSDKCrashReportSinkEMail.h"

#import "TTSDKCrashReport.h"
#import "TTSDKCrashReportFilterAppleFmt.h"
#import "TTSDKCrashReportFilterBasic.h"
#import "TTSDKCrashReportFilterGZip.h"
#import "TTSDKCrashReportFilterJSON.h"
#import "TTSDKNSErrorHelper.h"
#import "TTSDKSystemCapabilities.h"

// #define TTSDKLogger_LocalLevel TRACE
#import "TTSDKLogger.h"

#if TTSDKCRASH_HAS_MESSAGEUI
#import <MessageUI/MessageUI.h>

@interface TTSDKCrashMailProcess : NSObject <MFMailComposeViewControllerDelegate>

@property(nonatomic, readwrite, copy) NSArray<id<TTSDKCrashReport>> *reports;
@property(nonatomic, readwrite, copy) TTSDKCrashReportFilterCompletion onCompletion;

@property(nonatomic, readwrite, strong) UIViewController *dummyVC;

+ (TTSDKCrashMailProcess *)process;

- (void)startWithController:(MFMailComposeViewController *)controller
                    reports:(NSArray<id<TTSDKCrashReport>> *)reports
                filenameFmt:(NSString *)filenameFmt
               onCompletion:(TTSDKCrashReportFilterCompletion)onCompletion;

- (void)presentModalVC:(UIViewController *)vc;
- (void)dismissModalVC;

@end

@implementation TTSDKCrashMailProcess

+ (TTSDKCrashMailProcess *)process
{
    return [[self alloc] init];
}

- (void)startWithController:(MFMailComposeViewController *)controller
                    reports:(NSArray<id<TTSDKCrashReport>> *)reports
                filenameFmt:(NSString *)filenameFmt
               onCompletion:(TTSDKCrashReportFilterCompletion)onCompletion
{
    self.reports = [reports copy];
    self.onCompletion = onCompletion;

    controller.mailComposeDelegate = self;

    int i = 1;
    for (TTSDKCrashReportData *report in reports) {
        if ([report isKindOfClass:[TTSDKCrashReportData class]] == NO || report.value == nil) {
            TTSDKLOG_ERROR(@"Unexpected non-data report: %@", report);
            continue;
        }
        [controller addAttachmentData:report.value
                             mimeType:@"binary"
                             fileName:[NSString stringWithFormat:filenameFmt, i++]];
    }

    [self presentModalVC:controller];
}

- (void)mailComposeController:(__unused MFMailComposeViewController *)mailController
          didFinishWithResult:(MFMailComposeResult)result
                        error:(NSError *)error
{
    [self dismissModalVC];

    switch (result) {
        case MFMailComposeResultSent:
            ttsdkcrash_callCompletion(self.onCompletion, self.reports, nil);
            break;
        case MFMailComposeResultSaved:
            ttsdkcrash_callCompletion(self.onCompletion, self.reports, nil);
            break;
        case MFMailComposeResultCancelled:
            ttsdkcrash_callCompletion(self.onCompletion, self.reports,
                                   [TTSDKNSErrorHelper errorWithDomain:[[self class] description]
                                                               code:0
                                                        description:@"User cancelled"]);
            break;
        case MFMailComposeResultFailed:
            ttsdkcrash_callCompletion(self.onCompletion, self.reports, error);
            break;
        default: {
            ttsdkcrash_callCompletion(self.onCompletion, self.reports,
                                   [TTSDKNSErrorHelper errorWithDomain:[[self class] description]
                                                               code:0
                                                        description:@"Unknown MFMailComposeResult: %d", result]);
        }
    }
}

- (void)presentModalVC:(UIViewController *)vc
{
    self.dummyVC = [[UIViewController alloc] initWithNibName:nil bundle:nil];
    self.dummyVC.view = [[UIView alloc] init];

    UIWindow *window = [[[UIApplication sharedApplication] delegate] window];
    [window addSubview:self.dummyVC.view];

    if ([self.dummyVC respondsToSelector:@selector(presentViewController:animated:completion:)]) {
        [self.dummyVC presentViewController:vc animated:YES completion:nil];
    } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        [self.dummyVC presentModalViewController:vc animated:YES];
#pragma clang diagnostic pop
    }
}

- (void)dismissModalVC
{
    if ([self.dummyVC respondsToSelector:@selector(dismissViewControllerAnimated:completion:)]) {
        [self.dummyVC dismissViewControllerAnimated:YES
                                         completion:^{
                                             [self.dummyVC.view removeFromSuperview];
                                             self.dummyVC = nil;
                                         }];
    } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        [self.dummyVC dismissModalViewControllerAnimated:NO];
#pragma clang diagnostic pop
        [self.dummyVC.view removeFromSuperview];
        self.dummyVC = nil;
    }
}

@end

@interface TTSDKCrashReportSinkEMail ()

@property(nonatomic, readwrite, copy) NSArray *recipients;
@property(nonatomic, readwrite, copy) NSString *subject;
@property(nonatomic, readwrite, copy) NSString *message;
@property(nonatomic, readwrite, copy) NSString *filenameFmt;

@end

@implementation TTSDKCrashReportSinkEMail

- (instancetype)initWithRecipients:(NSArray<NSString *> *)recipients
                           subject:(NSString *)subject
                           message:(nullable NSString *)message
                       filenameFmt:(NSString *)filenameFmt
{
    if ((self = [super init])) {
        _recipients = [recipients copy];
        _subject = [subject copy];
        _message = [message copy];
        _filenameFmt = [filenameFmt copy];
    }
    return self;
}

- (id<TTSDKCrashReportFilter>)defaultCrashReportFilterSet
{
    return [[TTSDKCrashReportFilterPipeline alloc] initWithFilters:@[
        [[TTSDKCrashReportFilterJSONEncode alloc] initWithOptions:TTSDKJSONEncodeOptionSorted | TTSDKJSONEncodeOptionPretty],
        [[TTSDKCrashReportFilterGZipCompress alloc] initWithCompressionLevel:-1],
        self,
    ]];
}

- (id<TTSDKCrashReportFilter>)defaultCrashReportFilterSetAppleFmt
{
    return [[TTSDKCrashReportFilterPipeline alloc] initWithFilters:@[
        [[TTSDKCrashReportFilterAppleFmt alloc] initWithReportStyle:TTSDKAppleReportStyleSymbolicatedSideBySide],
        [TTSDKCrashReportFilterStringToData new],
        [[TTSDKCrashReportFilterGZipCompress alloc] initWithCompressionLevel:-1],
        self,
    ]];
}

- (void)filterReports:(NSArray<id<TTSDKCrashReport>> *)reports onCompletion:(TTSDKCrashReportFilterCompletion)onCompletion
{
    if (![MFMailComposeViewController canSendMail]) {
        UIAlertController *alertController =
            [UIAlertController alertControllerWithTitle:@"Email Error"
                                                message:@"This device is not configured to send email."
                                         preferredStyle:UIAlertControllerStyleAlert];
        UIAlertAction *okAction = [UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil];
        [alertController addAction:okAction];
        UIWindow *keyWindow = [[UIApplication sharedApplication] keyWindow];
        [keyWindow.rootViewController presentViewController:alertController animated:YES completion:NULL];

        ttsdkcrash_callCompletion(onCompletion, reports,
                               [TTSDKNSErrorHelper errorWithDomain:[[self class] description]
                                                           code:0
                                                    description:@"E-Mail not enabled on device"]);
        return;
    }

    MFMailComposeViewController *mailController = [[MFMailComposeViewController alloc] init];
    [mailController setToRecipients:self.recipients];
    [mailController setSubject:self.subject];
    if (self.message != nil) {
        [mailController setMessageBody:self.message isHTML:NO];
    }
    NSString *filenameFmt = self.filenameFmt;

    dispatch_async(dispatch_get_main_queue(), ^{
        __block TTSDKCrashMailProcess *process = [[TTSDKCrashMailProcess alloc] init];
        [process startWithController:mailController
                             reports:reports
                         filenameFmt:filenameFmt
                        onCompletion:^(NSArray *filteredReports, NSError *error) {
                            ttsdkcrash_callCompletion(onCompletion, filteredReports, error);
                            dispatch_async(dispatch_get_main_queue(), ^{
                                process = nil;
                            });
                        }];
    });
}

@end

#else

#import "TTSDKNSErrorHelper.h"

@implementation TTSDKCrashReportSinkEMail

+ (TTSDKCrashReportSinkEMail *)sinkWithRecipients:(NSArray *)recipients
                                       subject:(NSString *)subject
                                       message:(NSString *)message
                                   filenameFmt:(NSString *)filenameFmt
{
    return [[self alloc] initWithRecipients:recipients subject:subject message:message filenameFmt:filenameFmt];
}

- (id)initWithRecipients:(__unused NSArray *)recipients
                 subject:(__unused NSString *)subject
                 message:(__unused NSString *)message
             filenameFmt:(__unused NSString *)filenameFmt
{
    return [super init];
}

- (void)filterReports:(NSArray<id<TTSDKCrashReport>> *)reports onCompletion:(TTSDKCrashReportFilterCompletion)onCompletion
{
    for (id<TTSDKCrashReport> report in reports) {
        NSLog(@"Report\n%@", report);
    }
    ttsdkcrash_callCompletion(onCompletion, reports,
                           [TTSDKNSErrorHelper errorWithDomain:[[self class] description]
                                                       code:0
                                                description:@"Cannot send mail on this platform"]);
}

- (id<TTSDKCrashReportFilter>)defaultCrashReportFilterSet
{
    return [[TTSDKCrashReportFilterPipeline alloc] initWithFilters:@[
        [[TTSDKCrashReportFilterJSONEncode alloc] initWithOptions:TTSDKJSONEncodeOptionSorted | TTSDKJSONEncodeOptionPretty],
        [[TTSDKCrashReportFilterGZipCompress alloc] initWithCompressionLevel:-1],
        self,
    ]];
}

- (id<TTSDKCrashReportFilter>)defaultCrashReportFilterSetAppleFmt
{
    return [[TTSDKCrashReportFilterPipeline alloc] initWithFilters:@[
        [[TTSDKCrashReportFilterAppleFmt alloc] initWithReportStyle:TTSDKAppleReportStyleSymbolicatedSideBySide],
        [TTSDKCrashReportFilterStringToData new],
        [[TTSDKCrashReportFilterGZipCompress alloc] initWithCompressionLevel:-1],
        self,
    ]];
}

@end

#endif
