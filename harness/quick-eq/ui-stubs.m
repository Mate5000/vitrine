// UI-only harnesses exercise entry points without pretending to render audio.
#import "Shared/QuickEQ/QuickEQ.h"
NSString *SGQuickEQLocalize(NSString *key) { return key; }
UIViewController *SGQuickEQCreatePanel(void) {
    UIViewController *page = [UIViewController new];
    page.title = @"Audio EQ (harness stub)";
    return page;
}
void SGQuickEQPresentPanel(id responder, UIView *sourceView) {
    NSLog(@"[harness] Quick EQ tapped from %@", sourceView ?: responder);
}
