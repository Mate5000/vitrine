// Same HeaderElementsUnit/viewDidLayoutSubviews already established by Player.x
// and the recorded player tree. Add a 44 pt control only in unused header space;
// never move, hide or replace Spotify's close/menu buttons or playlist label.
#import "Core/SGCore.h"
#import "Shared/QuickEQ/QuickEQ.h"

static char kSGQuickEQButton;
static void placeQuickEQ(UIViewController *owner) {
    UIView *host = owner.viewIfLoaded;
    if (!host || host.bounds.size.height < 44) return;
    UIButton *button = objc_getAssociatedObject(host, &kSGQuickEQButton);
    if (!button) {
        button = [UIButton buttonWithType:UIButtonTypeSystem];
        [button setImage:[UIImage systemImageNamed:@"slider.horizontal.3"] forState:UIControlStateNormal];
        button.tintColor = UIColor.whiteColor;
        button.backgroundColor = [UIColor colorWithWhite:0 alpha:0.25];
        button.layer.cornerRadius = 22;
        button.accessibilityLabel = SGQuickEQLocalize(@"Audio EQ");
        button.accessibilityIdentifier = @"SGQuickEQ.player";
        __weak UIView *source = host;
        [button addAction:[UIAction actionWithHandler:^(__unused UIAction *action) { SGQuickEQPresentPanel(nil, source); }]
            forControlEvents:UIControlEventTouchUpInside];
        [host addSubview:button];
        objc_setAssociatedObject(host, &kSGQuickEQButton, button, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    NSMutableArray<NSValue *> *occupied = [NSMutableArray array];
    SGForEachView(host, ^(UIView *view) {
        if (view == host || view == button || [view isDescendantOfView:button] || view.hidden || view.alpha < 0.01) return;
        if ([view isKindOfClass:UIControl.class] || [view isKindOfClass:UILabel.class])
            [occupied addObject:[NSValue valueWithCGRect:CGRectInset([view convertRect:view.bounds toView:host], -4, 0)]];
    });
    const CGFloat width = CGRectGetWidth(host.bounds), y = (CGRectGetHeight(host.bounds) - 44) / 2;
    BOOL fits = NO;
    CGRect frame = CGRectZero;
    const BOOL rtl = host.effectiveUserInterfaceLayoutDirection == UIUserInterfaceLayoutDirectionRightToLeft;
    for (CGFloat inset = 56; inset + 44 <= width - 52; inset += 4) {
        frame = CGRectMake(rtl ? inset : width - inset - 44, y, 44, 44);
        fits = YES;
        for (NSValue *rect in occupied) if (CGRectIntersectsRect(frame, rect.CGRectValue)) { fits = NO; break; }
        if (fits) break;
    }
    button.alpha = fits ? 1 : 0;
    button.userInteractionEnabled = fits;
    button.accessibilityElementsHidden = !fits;
    if (fits) { button.frame = frame; [host bringSubviewToFront:button]; }
}
%hook _TtC20NowPlaying_ModesImpl18HeaderElementsUnit
- (void)viewDidLayoutSubviews { %orig; placeQuickEQ((UIViewController *)self); }
%end
%ctor {
    if (!SGNativeUI()) return;
    %init;
    SGRequireClasses(@[@"_TtC20NowPlaying_ModesImpl18HeaderElementsUnit"]);
}
