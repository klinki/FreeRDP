/**
 * macOS cooperative activation for decisions owned by the parent launcher.
 * Copyright 2026 FreeRDP contributors. Licensed under Apache-2.0.
 */
#import <AppKit/AppKit.h>
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
