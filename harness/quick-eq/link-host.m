// Link-check stand-ins. The actual tweak links Settings/SGPage.m and Core/SGGlass.m.
#import "Settings/SGPage.h"
#import "Core/SGGlass.h"
@implementation SGPage
@end
void SGPresentDark(UIViewController *controller) {
    controller.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
}
