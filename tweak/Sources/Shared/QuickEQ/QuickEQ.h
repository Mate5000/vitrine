#pragma once
#import <UIKit/UIKit.h>

#ifdef __cplusplus
extern "C" {
#endif
NSString *SGQuickEQLocalize(NSString *key);
void SGQuickEQInitializeSettings(void);
UIViewController *SGQuickEQCreatePanel(void);
void SGQuickEQPresentPanel(id responder, UIView *sourceView);
NSString *SGQuickEQAudioDiagnosticReport(void);
#ifdef __cplusplus
}
#endif
