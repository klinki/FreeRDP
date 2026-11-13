/**
 * macOS cooperative activation for decisions owned by the parent launcher.
 * Copyright 2026 FreeRDP contributors. Licensed under Apache-2.0.
 */
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#import <IOSurface/IOSurface.h>
#import <CoreVideo/CoreVideo.h>
#include <mach/mach.h>
#include <servers/bootstrap.h>
#include <map>
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
#if __has_feature(objc_arc)
#error This file assumes manual reference counting.
#endif

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
			/* alloc/init: this file is built without ARC, so autoreleased objects would dangle. */
			hostedContexts = [[NSMutableDictionary alloc] init];
			hostedViews = [[NSMutableDictionary alloc] init];
			/* No visible window: keep App Nap from throttling the session. */
			hostedActivity = [[[NSProcessInfo processInfo]
			    beginActivityWithOptions:NSActivityUserInitiated | NSActivityLatencyCritical
			                      reason:@"Hosted remote desktop session"] retain];
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
		if (!layer)
			return;
		if (view)
			[view setFrameSize:NSMakeSize(width, height)];
		[CATransaction begin];
		[CATransaction setDisableActions:YES];
		layer.bounds = CGRectMake(0, 0, width, height);
		layer.contentsScale = scale;
		layer.drawableSize = CGSizeMake(width * scale, height * scale);
		[CATransaction commit];
	}
}

/* Spike, IOSurface variant. Message layout shared with the probe host (spikeipc.h in the
 * Sheltie repository's evidence folder). */
typedef struct
{
	mach_msg_header_t header;
	mach_msg_body_t body;
	mach_msg_port_descriptor_t surface;
	uint32_t index, width, height;
} SpikeSurfaceMessage;
typedef struct
{
	mach_msg_header_t header;
	uint32_t index;
	uint64_t frame, sentNs;
} SpikeFrameMessage;
enum { SpikeSurfaceId = 1, SpikeFrameId = 2, SpikeSurfaceCount = 3 };

struct SpikeSurfaces
{
	IOSurfaceRef surface[SpikeSurfaceCount] = {};
	SDL_Texture* texture[SpikeSurfaceCount] = {};
	int width = 0, height = 0;
	unsigned current = 0;
	uint64_t frame = 0;
};
static mach_port_t surfaceHost = MACH_PORT_NULL;
static std::map<uint32_t, SpikeSurfaces> surfaceSets;

bool SdlLauncher::connectSurfaceHost(const char* service)
{
	if (surfaceHost != MACH_PORT_NULL)
		return true;
	const kern_return_t kr = bootstrap_look_up(bootstrap_port, service, &surfaceHost);
	fprintf(stderr, "[surface] look up %s: %s\n", service, mach_error_string(kr));
	if (kr == KERN_SUCCESS && !hostedActivity)
		hostedActivity = [[[NSProcessInfo processInfo]
		    beginActivityWithOptions:NSActivityUserInitiated | NSActivityLatencyCritical
		                      reason:@"Hosted remote desktop session"] retain];
	return kr == KERN_SUCCESS;
}

SDL_Texture* SdlLauncher::surfaceTarget(SDL_Window* window, SDL_Renderer* renderer)
{
	if (surfaceHost == MACH_PORT_NULL)
		return nullptr;
	int width = 0, height = 0;
	if (!SDL_GetRenderOutputSize(renderer, &width, &height) || width <= 0 || height <= 0)
		return nullptr;
	auto& set = surfaceSets[SDL_GetWindowID(window)];
	if (set.width != width || set.height != height)
	{
		for (unsigned i = 0; i < SpikeSurfaceCount; i++)
		{
			if (set.texture[i]) SDL_DestroyTexture(set.texture[i]);
			if (set.surface[i]) CFRelease(set.surface[i]);
			NSDictionary* props = @{ (id)kIOSurfaceWidth : @(width), (id)kIOSurfaceHeight : @(height),
				                     (id)kIOSurfaceBytesPerElement : @4,
				                     (id)kIOSurfacePixelFormat : @((uint32_t)'BGRA') };
			set.surface[i] = IOSurfaceCreate((CFDictionaryRef)props);
			CVPixelBufferRef buffer = nullptr;
			CVPixelBufferCreateWithIOSurface(kCFAllocatorDefault, set.surface[i], nullptr, &buffer);
			SDL_PropertiesID p = SDL_CreateProperties();
			SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_ARGB8888);
			SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_TARGET);
			SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, width);
			SDL_SetNumberProperty(p, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, height);
			SDL_SetPointerProperty(p, SDL_PROP_TEXTURE_CREATE_METAL_PIXELBUFFER_POINTER, buffer);
			set.texture[i] = SDL_CreateTextureWithProperties(renderer, p);
			SDL_DestroyProperties(p);
			if (buffer) CVPixelBufferRelease(buffer);
			SpikeSurfaceMessage m = {};
			m.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) | MACH_MSGH_BITS_COMPLEX;
			m.header.msgh_size = sizeof(m);
			m.header.msgh_remote_port = surfaceHost;
			m.header.msgh_id = SpikeSurfaceId;
			m.body.msgh_descriptor_count = 1;
			m.surface.name = IOSurfaceCreateMachPort(set.surface[i]);
			m.surface.disposition = MACH_MSG_TYPE_COPY_SEND;
			m.surface.type = MACH_MSG_PORT_DESCRIPTOR;
			m.index = i; m.width = static_cast<uint32_t>(width); m.height = static_cast<uint32_t>(height);
			mach_msg(&m.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof(m), 0, MACH_PORT_NULL, 100, MACH_PORT_NULL);
			mach_port_deallocate(mach_task_self(), m.surface.name);
		}
		set.width = width; set.height = height;
		fprintf(stderr, "[surface] window %u surfaces %dx%d\n", SDL_GetWindowID(window), width, height);
	}
	return set.texture[set.current];
}

void SdlLauncher::surfacePresent(SDL_Window* window)
{
	const auto found = surfaceSets.find(SDL_GetWindowID(window));
	if (found == surfaceSets.end() || surfaceHost == MACH_PORT_NULL)
		return;
	auto& set = found->second;
	SpikeFrameMessage f = {};
	f.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	f.header.msgh_size = sizeof(f);
	f.header.msgh_remote_port = surfaceHost;
	f.header.msgh_id = SpikeFrameId;
	f.index = set.current; f.frame = set.frame++;
	f.sentNs = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
	mach_msg(&f.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof(f), 0, MACH_PORT_NULL, 5, MACH_PORT_NULL);
	set.current = (set.current + 1) % SpikeSurfaceCount;
}
