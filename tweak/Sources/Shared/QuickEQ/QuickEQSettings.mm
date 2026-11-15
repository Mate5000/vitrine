#import "QuickEQ.h"
#import "QuickEQInternal.hpp"
#import "Settings/SGPage.h"
#import "Core/SGGlass.h"
#import <AVFoundation/AVFoundation.h>

static NSUserDefaults *preferences;
static NSBundle *resources;
static sgquickeq::Configuration configuration;
sgquickeq::Mailbox &SGQuickEQConfigurationMailbox() {
    // Initialize before publishing restored settings, independent of dylib/C++
    // constructor order. Audio hooks are installed only after this is warmed.
    static sgquickeq::Mailbox mailbox;
    return mailbox;
}
sgquickeq::Configuration SGQuickEQCurrentConfiguration() { return configuration; } // main thread only

NSString *SGQuickEQLocalize(NSString *key) {
    return [resources localizedStringForKey:key value:key table:nil] ?: key;
}
static float savedNumber(NSDictionary *dictionary, NSString *key, float fallback) {
    id value = dictionary[key];
    return [value isKindOfClass:NSNumber.class] ? [value floatValue] : fallback;
}
void SGQuickEQInitializeSettings() {
    preferences = NSUserDefaults.standardUserDefaults;
    NSString *path = [NSBundle.mainBundle pathForResource:@"SGQuickEQ" ofType:@"bundle"];
    if (!path) {
        for (NSString *prefix in @[@"/Library/Application Support", @"/var/jb/Library/Application Support"]) {
            NSString *candidate = [prefix stringByAppendingPathComponent:@"SGQuickEQ.bundle"];
            if ([NSFileManager.defaultManager fileExistsAtPath:candidate]) { path = candidate; break; }
        }
    }
    resources = path ? [NSBundle bundleWithPath:path] : nil;
    NSDictionary *saved = [preferences dictionaryForKey:@"spotifyglass.quickEQ.configuration"];
    configuration = {};
    configuration.enabled = savedNumber(saved, @"enabled", 0) != 0;
    configuration.bassEnabled = savedNumber(saved, @"bassEnabled", 0) != 0;
    configuration.eqEnabled = savedNumber(saved, @"eqEnabled", 0) != 0;
    configuration.bassFrequency = savedNumber(saved, @"bassFrequency", 80);
    configuration.bassGain = savedNumber(saved, @"bassGain", 4);
    configuration.bassMaxGain = savedNumber(saved, @"bassMaxGain", 12);
    NSArray *bands = saved[@"bands"];
    if ([bands isKindOfClass:NSArray.class] && bands.count == sgquickeq::BandCount) {
        for (NSUInteger i = 0; i < bands.count; ++i)
            if ([bands[i] isKindOfClass:NSNumber.class]) configuration.bands[i] = [bands[i] floatValue];
    }
    configuration = sgquickeq::sanitize(configuration);
    SGQuickEQConfigurationMailbox().publish(configuration);
}
void SGQuickEQSaveConfiguration(sgquickeq::Configuration value) {
    NSCAssert(NSThread.isMainThread, @"EQ settings must be serialized on the main thread");
    configuration = sgquickeq::sanitize(value);
    NSMutableArray *bands = [NSMutableArray arrayWithCapacity:sgquickeq::BandCount];
    for (float gain : configuration.bands) [bands addObject:@(gain)];
    [preferences setObject:@{
        @"enabled": @(configuration.enabled), @"bassEnabled": @(configuration.bassEnabled),
        @"eqEnabled": @(configuration.eqEnabled), @"bassFrequency": @(configuration.bassFrequency),
        @"bassGain": @(configuration.bassGain), @"bassMaxGain": @(configuration.bassMaxGain), @"bands": bands
    } forKey:@"spotifyglass.quickEQ.configuration"];
    SGQuickEQConfigurationMailbox().publish(configuration);
}

static NSString *fourCC(uint32_t value) {
    char code[5] = {};
    for (unsigned i = 0; i < 4; ++i) {
        const char c = char((value >> ((3 - i) * 8)) & 255);
        code[i] = c >= 32 && c <= 126 ? c : '.';
    }
    return [NSString stringWithUTF8String:code];
}
static NSString *errorDescription(uint64_t value) {
    const int32_t status = sgquickeq::failureStatus(value);
    return [NSString stringWithFormat:@"%d / 0x%08x / '%@'", status, uint32_t(status), fourCC(uint32_t(status))];
}
NSString *SGQuickEQAudioDiagnosticReport() {
    const auto &s = SGQuickEQAudioStatistics();
    AVAudioSession *session = AVAudioSession.sharedInstance;
    NSMutableArray *routes = [NSMutableArray array];
    for (AVAudioSessionPortDescription *port in session.currentRoute.outputs)
        [routes addObject:[NSString stringWithFormat:@"%@ (%@)", port.portType, port.portName]];
    return [NSString stringWithFormat:@"Vitrine Quick EQ 1.0.0\nSpotify: %@ / iOS: %@\nMaster / Bass / EQ: %d / %d / %d\nBass: %.0f Hz / %.1f dB / max %.1f dB\nOutput: %@\nDevice: %.0f Hz\nHook reachable: %d\nCallbacks / processed buffers / frames: %llu / %llu / %llu\nUnsupported / concurrent: %llu / %llu\nPCM rate: %u Hz\nLast failure: %s / %@\nZero processed frames means the EQ has not processed audio. AirPlay and Spotify Connect are outside this feature.",
        [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"] ?: @"?", UIDevice.currentDevice.systemVersion,
        configuration.enabled, configuration.bassEnabled, configuration.eqEnabled, configuration.bassFrequency, configuration.bassGain, configuration.bassMaxGain,
        [routes componentsJoinedByString:@", "], session.sampleRate, s.reachable.load(),
        (unsigned long long)s.callbacks.load(), (unsigned long long)s.processedBlocks.load(), (unsigned long long)s.processedFrames.load(),
        (unsigned long long)s.unsupportedBlocks.load(), (unsigned long long)s.concurrentBlocks.load(), s.sampleRate.load(),
        sgquickeq::failureStageName(sgquickeq::failureStage(s.lastFailure.load())), errorDescription(s.lastFailure.load())];
}

@interface SGQuickEQDiagnosticsViewController : UIViewController
@property(nonatomic, strong) UITextView *textView;
@property(nonatomic, strong) NSTimer *timer;
@end
@implementation SGQuickEQDiagnosticsViewController
- (void)dealloc { [_timer invalidate]; }
- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = SGQuickEQLocalize(@"Audio diagnostics");
    self.view.backgroundColor = UIColor.systemBackgroundColor;
    self.textView = [[UITextView alloc] initWithFrame:CGRectZero];
    self.textView.editable = NO;
    self.textView.selectable = YES;
    self.textView.font = [UIFont monospacedSystemFontOfSize:14 weight:UIFontWeightRegular];
    self.textView.adjustsFontForContentSizeCategory = YES;
    self.textView.textColor = UIColor.labelColor;
    self.textView.backgroundColor = UIColor.systemBackgroundColor;
    self.textView.accessibilityIdentifier = @"SGQuickEQ.diagnostics";
    self.textView.translatesAutoresizingMaskIntoConstraints = NO;
    self.textView.textContainerInset = UIEdgeInsetsMake(16, 12, 24, 12);
    [self.view addSubview:self.textView];
    UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [self.textView.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor],
        [self.textView.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor],
        [self.textView.topAnchor constraintEqualToAnchor:safe.topAnchor],
        [self.textView.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor]
    ]];
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:SGQuickEQLocalize(@"Copy diagnostics") style:UIBarButtonItemStylePlain target:self action:@selector(copyDiagnostics)];
    if (self.navigationController.viewControllers.firstObject == self)
        self.navigationItem.leftBarButtonItem = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone target:self action:@selector(closeDiagnostics)];
}
- (void)refreshDiagnostics {
    if (self.textView.selectedRange.length) return;
    CGPoint offset = self.textView.contentOffset;
    self.textView.text = SGQuickEQAudioDiagnosticReport();
    [self.textView setContentOffset:offset animated:NO];
}
- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self.timer invalidate];
    [self refreshDiagnostics];
    __weak SGQuickEQDiagnosticsViewController *weakSelf = self;
    self.timer = [NSTimer scheduledTimerWithTimeInterval:1 repeats:YES block:^(__unused NSTimer *timer) { [weakSelf refreshDiagnostics]; }];
}
- (void)viewDidDisappear:(BOOL)animated { [super viewDidDisappear:animated]; [self.timer invalidate]; self.timer = nil; }
- (void)copyDiagnostics { UIPasteboard.generalPasteboard.string = SGQuickEQAudioDiagnosticReport(); }
- (void)closeDiagnostics { [self dismissViewControllerAnimated:YES completion:nil]; }
@end

@interface SGQuickEQSliderCell : UITableViewCell
@property(nonatomic, strong) UILabel *valueLabel;
@property(nonatomic, strong) UISlider *slider;
@end
@implementation SGQuickEQSliderCell
- (instancetype)initWithStyle:(UITableViewCellStyle)style reuseIdentifier:(NSString *)identifier {
    self = [super initWithStyle:style reuseIdentifier:identifier];
    if (!self) return nil;
    self.selectionStyle = UITableViewCellSelectionStyleNone;
    _valueLabel = [UILabel new];
    _valueLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    _valueLabel.adjustsFontForContentSizeCategory = YES;
    _valueLabel.numberOfLines = 0;
    _slider = [UISlider new];
    _slider.minimumTrackTintColor = [UIColor colorWithRed:0.65 green:0.35 blue:0.85 alpha:1];
    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:@[_valueLabel, _slider]];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 6;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [self.contentView addSubview:stack];
    UILayoutGuide *margins = self.contentView.layoutMarginsGuide;
    [NSLayoutConstraint activateConstraints:@[
        [stack.leadingAnchor constraintEqualToAnchor:margins.leadingAnchor],
        [stack.trailingAnchor constraintEqualToAnchor:margins.trailingAnchor],
        [stack.topAnchor constraintEqualToAnchor:margins.topAnchor constant:4],
        [stack.bottomAnchor constraintEqualToAnchor:margins.bottomAnchor constant:-4],
        [_slider.heightAnchor constraintGreaterThanOrEqualToConstant:44]
    ]];
    return self;
}
@end

@interface SGQuickEQViewController : SGPage
@property(nonatomic, strong) NSTimer *statusTimer;
@property(nonatomic) uint64_t lastBlocks;
@end

@implementation SGQuickEQViewController
- (void)dealloc { [_statusTimer invalidate]; }
- (instancetype)init { return [super initWithStyle:UITableViewStyleInsetGrouped]; }
- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = SGQuickEQLocalize(@"Audio EQ");
    self.tableView.rowHeight = UITableViewAutomaticDimension;
    self.tableView.estimatedRowHeight = 76;
    self.tableView.accessibilityIdentifier = @"SGQuickEQ.panel";
    if (self.navigationController.viewControllers.firstObject == self)
        self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone target:self action:@selector(closePanel)];
}
- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self.statusTimer invalidate];
    __weak SGQuickEQViewController *weakSelf = self;
    self.statusTimer = [NSTimer scheduledTimerWithTimeInterval:1 repeats:YES block:^(__unused NSTimer *timer) { [weakSelf updateStatus]; }];
    [self.tableView reloadData];
}
- (void)viewDidDisappear:(BOOL)animated {
    [super viewDidDisappear:animated];
    [self.statusTimer invalidate]; self.statusTimer = nil;
}
- (void)closePanel { [self dismissViewControllerAnimated:YES completion:nil]; }
- (NSInteger)numberOfSectionsInTableView:(__unused UITableView *)tableView { return 5; }
- (NSInteger)tableView:(__unused UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    return section == 0 ? 2 : section == 1 ? 4 : section == 2 ? 11 : 2;
}
- (NSString *)tableView:(__unused UITableView *)tableView titleForHeaderInSection:(NSInteger)section {
    return SGQuickEQLocalize(@[@"Audio EQ", @"Bass boost", @"10-band equalizer", @"Reset", @"Diagnostics"][section]);
}
- (NSString *)tableView:(__unused UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    if (section == 0) return SGQuickEQLocalize(@"Changes are saved automatically and apply to all tracks. Enable Bass boost or Equalizer as well as the master switch.");
    if (section == 1) return SGQuickEQLocalize(@"A smooth bass shelf. Max gain limits the Bass gain slider; it is not an output volume control.");
    if (section == 2) return SGQuickEQLocalize(@"Automatic headroom and a -1 dBFS sample-peak limiter protect the digital signal. Some overall volume reduction is normal. AirPlay and Spotify Connect are not supported.");
    return nil;
}
- (NSString *)statusText {
    const auto &stats = SGQuickEQAudioStatistics();
    if (!configuration.enabled) return SGQuickEQLocalize(@"Quick EQ off");
    bool effect = configuration.bassEnabled && configuration.bassGain > 0;
    for (float band : configuration.bands) effect = effect || (configuration.eqEnabled && band != 0);
    if (!effect) return SGQuickEQLocalize(@"Quick EQ flat");
    if (!stats.reachable.load()) return SGQuickEQLocalize(@"Spotify audio hook unavailable");
    const uint64_t blocks = stats.processedBlocks.load();
    const uint64_t failure = stats.currentFailure.load();
    NSString *status;
    if (failure) {
        status = [NSString stringWithFormat:@"%@\n%s\n%@", SGQuickEQLocalize(@"Quick EQ unavailable — audio passes through"), sgquickeq::failureStageName(sgquickeq::failureStage(failure)), errorDescription(failure)];
    } else if (blocks > self.lastBlocks) {
        NSString *backend = @"Spotify PCM";
        status = [NSString stringWithFormat:SGQuickEQLocalize(@"Active: %@, %u Hz, %llu processed buffers"), backend, stats.sampleRate.load(), (unsigned long long)blocks];
    } else status = SGQuickEQLocalize(@"Waiting for audio — processing has not been confirmed");
    self.lastBlocks = blocks;
    return status;
}
- (void)updateStatus {
    UITableViewCell *cell = [self.tableView cellForRowAtIndexPath:[NSIndexPath indexPathForRow:1 inSection:0]];
    NSString *text = [self statusText];
    if (cell && ![cell.textLabel.text isEqualToString:text]) {
        cell.textLabel.text = text;
        [UIView performWithoutAnimation:^{ [self.tableView beginUpdates]; [self.tableView endUpdates]; }];
    }
}
- (UITableViewCell *)tableView:(__unused UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    const NSInteger section = indexPath.section, row = indexPath.row;
    if ((section == 0 && row == 0) || (section == 1 && row == 0) || (section == 2 && row == 0)) {
        UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
        cell.textLabel.text = SGQuickEQLocalize(section == 0 ? @"Enable Audio EQ" : section == 1 ? @"Enable bass boost" : @"Enable equalizer");
        cell.textLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
        cell.textLabel.adjustsFontForContentSizeCategory = YES;
        cell.textLabel.numberOfLines = 0;
        UISwitch *toggle = [UISwitch new];
        toggle.tag = section;
        toggle.on = section == 0 ? configuration.enabled : section == 1 ? configuration.bassEnabled : configuration.eqEnabled;
        toggle.accessibilityLabel = cell.textLabel.text;
        toggle.accessibilityIdentifier = [NSString stringWithFormat:@"SGQuickEQ.switch.%ld", (long)section];
        [toggle addTarget:self action:@selector(toggleChanged:) forControlEvents:UIControlEventValueChanged];
        cell.accessoryView = toggle; cell.selectionStyle = UITableViewCellSelectionStyleNone;
        return cell;
    }
    if (section == 1 || section == 2) {
        SGQuickEQSliderCell *cell = [[SGQuickEQSliderCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
        UISlider *slider = cell.slider;
        slider.tag = section == 1 ? row : row + 9;
        slider.minimumValue = section == 1 ? (row == 1 ? 30 : 0) : -12;
        slider.maximumValue = section == 1 ? (row == 1 ? 200 : row == 2 ? MAX(0.01f, configuration.bassMaxGain) : 18) : 12;
        slider.value = section == 1 ? (row == 1 ? configuration.bassFrequency : row == 2 ? configuration.bassGain : configuration.bassMaxGain) : configuration.bands[row - 1];
        slider.enabled = !(section == 1 && row == 2 && configuration.bassMaxGain == 0);
        slider.accessibilityIdentifier = [NSString stringWithFormat:@"SGQuickEQ.slider.%ld", (long)slider.tag];
        [slider addTarget:self action:@selector(sliderChanged:) forControlEvents:UIControlEventValueChanged];
        [self updateSliderLabel:cell];
        return cell;
    }
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
    cell.textLabel.numberOfLines = 0;
    cell.textLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    cell.textLabel.adjustsFontForContentSizeCategory = YES;
    if (section == 0) {
        cell.textLabel.text = [self statusText]; cell.textLabel.textColor = UIColor.secondaryLabelColor;
        cell.selectionStyle = UITableViewCellSelectionStyleNone;
    } else if (section == 4) {
        cell.textLabel.text = SGQuickEQLocalize(row == 0 ? @"Show detailed diagnostics" : @"Copy diagnostics");
        cell.textLabel.textColor = self.view.tintColor;
        if (row == 0) cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    } else {
        cell.textLabel.text = SGQuickEQLocalize(row == 0 ? @"Reset equalizer to flat" : @"Reset all audio settings");
        cell.textLabel.textColor = self.view.tintColor;
    }
    return cell;
}
- (void)updateSliderLabel:(SGQuickEQSliderCell *)cell {
    NSInteger tag = cell.slider.tag;
    NSString *name, *value;
    if (tag == 1) { name = SGQuickEQLocalize(@"Frequency"); value = [NSString stringWithFormat:@"%.0f Hz", configuration.bassFrequency]; }
    else if (tag == 2 || tag == 3) {
        name = SGQuickEQLocalize(tag == 2 ? @"Bass gain" : @"Max gain");
        value = [NSString stringWithFormat:@"%.1f dB", tag == 2 ? configuration.bassGain : configuration.bassMaxGain];
    } else {
        NSUInteger band = tag - 10;
        double f = sgquickeq::Frequencies[band];
        name = f >= 1000 ? [NSString stringWithFormat:@"%g kHz", f / 1000] : [NSString stringWithFormat:@"%g Hz", f];
        value = [NSString stringWithFormat:@"%+.1f dB", configuration.bands[band]];
    }
    cell.valueLabel.text = [NSString stringWithFormat:@"%@: %@", name, value];
    cell.slider.accessibilityLabel = name; cell.slider.accessibilityValue = value;
}
- (void)toggleChanged:(UISwitch *)toggle {
    auto next = configuration;
    if (toggle.tag == 0) next.enabled = toggle.on;
    else if (toggle.tag == 1) next.bassEnabled = toggle.on;
    else next.eqEnabled = toggle.on;
    SGQuickEQSaveConfiguration(next); [self updateStatus];
}
- (void)sliderChanged:(UISlider *)slider {
    auto next = configuration;
    float value = roundf(slider.value * (slider.tag == 1 ? 1 : 10)) / (slider.tag == 1 ? 1 : 10);
    if (slider.tag == 1) next.bassFrequency = value;
    else if (slider.tag == 2) next.bassGain = value;
    else if (slider.tag == 3) next.bassMaxGain = value;
    else next.bands[slider.tag - 10] = value;
    SGQuickEQSaveConfiguration(next);
    UIView *view = slider.superview;
    while (view && ![view isKindOfClass:SGQuickEQSliderCell.class]) view = view.superview;
    if (view) [self updateSliderLabel:(SGQuickEQSliderCell *)view];
    if (slider.tag == 3) {
        SGQuickEQSliderCell *gain = [self.tableView cellForRowAtIndexPath:[NSIndexPath indexPathForRow:2 inSection:1]];
        gain.slider.maximumValue = MAX(0.01f, configuration.bassMaxGain);
        gain.slider.value = configuration.bassGain;
        gain.slider.enabled = configuration.bassMaxGain > 0;
        if (gain) [self updateSliderLabel:gain];
    }
}
- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    if (indexPath.section == 4) {
        if (indexPath.row == 0) {
            UIViewController *diagnostics = [SGQuickEQDiagnosticsViewController new];
            // Spotify's navigation stack only accepts SPTPageController pages.
            // The text report lives in its own system navigation controller.
            UINavigationController *navigation = [[UINavigationController alloc] initWithRootViewController:diagnostics];
            SGPresentDark(navigation);
            [self presentViewController:navigation animated:YES completion:nil];
        }
        else UIPasteboard.generalPasteboard.string = SGQuickEQAudioDiagnosticReport();
        return;
    }
    if (indexPath.section != 3) return;
    auto next = configuration;
    if (indexPath.row == 0) next.bands = {};
    else next = {};
    SGQuickEQSaveConfiguration(next); [tableView reloadData];
}
@end

UIViewController *SGQuickEQCreatePanel() { return [SGQuickEQViewController new]; }
static void presentPanelInWindow(UIWindow *window, NSUInteger attempt) {
    if (!window || window.hidden) return;
    UIViewController *presenter = window.rootViewController;
    while (presenter.presentedViewController) presenter = presenter.presentedViewController;
    if (!presenter || [presenter isKindOfClass:SGQuickEQViewController.class] ||
        ([presenter isKindOfClass:UINavigationController.class] && [((UINavigationController *)presenter).topViewController isKindOfClass:SGQuickEQViewController.class])) return;
    // Like Vitrine's existing popover presenter, wait for the system menu/player
    // transition. Presenting from a menu which is closing can lose the EQ sheet.
    // Re-resolve the controller in the same window after each transition tick.
    if (presenter.isBeingDismissed || presenter.isBeingPresented || presenter.transitionCoordinator) {
        if (attempt < 20) {
            __weak UIWindow *weakWindow = window;
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.05 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
                presentPanelInWindow(weakWindow, attempt + 1);
            });
        }
        return;
    }
    UINavigationController *navigation = [[UINavigationController alloc] initWithRootViewController:SGQuickEQCreatePanel()];
    navigation.modalPresentationStyle = UIModalPresentationPageSheet;
    SGPresentDark(navigation);
    [presenter presentViewController:navigation animated:YES completion:nil];
}
void SGQuickEQPresentPanel(id responder, UIView *sourceView) {
    __weak id weakResponder = responder;
    __weak UIView *weakView = sourceView;
    dispatch_async(dispatch_get_main_queue(), ^{
        id candidate = weakResponder ?: weakView;
        UIViewController *owner = nil;
        for (NSUInteger depth = 0; candidate && depth < 32; ++depth) {
            if ([candidate isKindOfClass:UIViewController.class]) { owner = candidate; break; }
            if ([candidate respondsToSelector:@selector(nextResponder)]) candidate = [candidate nextResponder];
            else break;
        }
        UIWindow *window = weakView.window ?: owner.view.window;
        if (!window) {
            for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
                if (scene.activationState != UISceneActivationStateForegroundActive || ![scene isKindOfClass:UIWindowScene.class]) continue;
                for (UIWindow *item in ((UIWindowScene *)scene).windows) if (item.isKeyWindow) { window = item; break; }
            }
        }
        presentPanelInWindow(window, 0);
    });
}
