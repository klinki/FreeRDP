/**
 * macOS cooperative activation for decisions owned by the parent launcher.
 * Copyright 2026 FreeRDP contributors. Licensed under Apache-2.0.
 */
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#include <SDL3/SDL.h>
#include <unistd.h>
#include "sdl_launcher.hpp"

bool SdlLauncher::useAccessoryActivationPolicy()
{
	if (![NSThread isMainThread] || !NSApp)
		return false;
	if ([NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory])
		return true;
	[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
	return false;
}

void SdlLauncher::activateAccessorySession()
{
	if ([NSThread isMainThread] &&
	    [NSApp activationPolicy] == NSApplicationActivationPolicyAccessory)
		[NSApp activateIgnoringOtherApps:YES];
}

void SdlLauncher::yieldActivationToLauncher()
{
	// requestClose runs on SDL's main thread. Only the foreground app can
	// authorize the launcher to take focus in response to the user's click.
	if (![NSThread isMainThread] || ![NSApp isActive])
		return;
	if (@available(macOS 14.0, *))
	{
		NSRunningApplication* launcher =
		    [NSRunningApplication runningApplicationWithProcessIdentifier:getppid()];
		if (launcher && ![launcher isTerminated])
			[NSApp yieldActivationToApplication:launcher];
	}
}

/* Spike for sheltie-macos#13: remote layer hosting. Private API, as used by Chromium. */
typedef uint32_t CGSConnectionID;
extern "C" CGSConnectionID CGSMainConnectionID(void);
@interface CAContext : NSObject
+ (instancetype)contextWithCGSConnection:(CGSConnectionID)connection options:(NSDictionary*)options;
@property(readonly) uint32_t contextId;
@property(strong) CALayer* layer;
@end

/* Contexts and detached views must outlive the session; keyed by SDL window ID. */
static NSMutableDictionary<NSNumber*, CAContext*>* hostedContexts;
static NSMutableDictionary<NSNumber*, NSView*>* hostedViews;
static id hostedActivity;

uint32_t SdlLauncher::exportLayer(SDL_Window* window, SDL_Renderer* renderer)
{
	@autoreleasepool
	{
		CALayer* layer = (__bridge CALayer*)SDL_GetRenderMetalLayer(renderer);
		NSWindow* nswindow = (__bridge NSWindow*)SDL_GetPointerProperty(
		    SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
		Class cls = NSClassFromString(@"CAContext");
		if (!layer || !nswindow || !cls ||
		    ![cls respondsToSelector:@selector(contextWithCGSConnection:options:)])
			return 0;
		if (!hostedContexts)
		{
			hostedContexts = [NSMutableDictionary dictionary];
			hostedViews = [NSMutableDictionary dictionary];
			/* No visible window: keep App Nap from throttling the session. */
			hostedActivity = [[NSProcessInfo processInfo]
			    beginActivityWithOptions:NSActivityUserInitiated | NSActivityLatencyCritical
			                      reason:@"Hosted remote desktop session"];
		}
		NSNumber* key = @(SDL_GetWindowID(window));
		/* While SDL's Metal view is in the window the exported layer shows nothing. */
		NSMutableArray<NSView*>* stack = [NSMutableArray arrayWithObject:nswindow.contentView];
		while (stack.count)
		{
			NSView* view = stack.lastObject;
			[stack removeLastObject];
			if (view.layer == layer)
			{
				hostedViews[key] = view;
				[view removeFromSuperview];
				break;
			}
			[stack addObjectsFromArray:view.subviews];
		}
		if (nswindow.contentView.layer == layer || !hostedViews[key])
			return 0;
		[layer removeFromSuperlayer];
		layer.anchorPoint = CGPointZero;
		layer.position = CGPointZero;
		CAContext* context = [cls contextWithCGSConnection:CGSMainConnectionID() options:@{}];
		context.layer = layer;
		[CATransaction flush];
		hostedContexts[key] = context;
		return context.contextId;
	}
}

void SdlLauncher::resizeLayer(SDL_Window* window, SDL_Renderer* renderer, int width, int height,
                              double scale)
{
	@autoreleasepool
	{
		CAMetalLayer* layer = (__bridge CAMetalLayer*)SDL_GetRenderMetalLayer(renderer);
		NSView* view = hostedViews[@(SDL_GetWindowID(window))];
		if (!layer || !view)
			return;
		[view setFrameSize:NSMakeSize(width, height)];
		[CATransaction begin];
		[CATransaction setDisableActions:YES];
		layer.bounds = CGRectMake(0, 0, width, height);
		layer.contentsScale = scale;
		layer.drawableSize = CGSizeMake(width * scale, height * scale);
		[CATransaction commit];
	}
}
