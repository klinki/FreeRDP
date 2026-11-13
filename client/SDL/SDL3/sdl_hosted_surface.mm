/** Hosted rendering output; see sdl_hosted_surface.hpp. Public API only. */
#include "sdl_hosted_surface.hpp"
#include "sdl_launcher_surface.h"

#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>

SdlHostedSurfaces& SdlHostedSurfaces::instance()
{
	static SdlHostedSurfaces surfaces;
	return surfaces;
}

bool SdlHostedSurfaces::adoptInheritedPort()
{
	mach_port_array_t ports = nullptr;
	mach_msg_type_number_t count = 0;
	if (mach_ports_lookup(mach_task_self(), &ports, &count) != KERN_SUCCESS || !ports)
		return false;
	mach_port_t found = MACH_PORT_NULL;
	for (mach_msg_type_number_t i = 0; i < count; i++)
	{
		if (i == SDL_LAUNCHER_SURFACE_PORT_SLOT && MACH_PORT_VALID(ports[i]))
			found = ports[i];
		else if (MACH_PORT_VALID(ports[i]))
			mach_port_deallocate(mach_task_self(), ports[i]);
	}
	vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(ports),
	              count * sizeof(mach_port_t));
	if (found == MACH_PORT_NULL)
		return false;
	/* Only send rights are of use; anything else in the slot is not the launcher's port. */
	mach_port_type_t type = 0;
	if (mach_port_type(mach_task_self(), found, &type) != KERN_SUCCESS ||
	    (type & MACH_PORT_TYPE_SEND) == 0)
	{
		mach_port_deallocate(mach_task_self(), found);
		return false;
	}
	if (_port != 0)
		mach_port_deallocate(mach_task_self(), _port);
	_port = found;
	_dead = false;
	return true;
}

void SdlHostedSurfaces::setPort(uint32_t sendRight)
{
	if (_port != 0)
		mach_port_deallocate(mach_task_self(), _port);
	_port = 0;
	_dead = false;
	if (MACH_PORT_VALID(sendRight) &&
	    mach_port_mod_refs(mach_task_self(), sendRight, MACH_PORT_RIGHT_SEND, 1) == KERN_SUCCESS)
		_port = sendRight;
}

bool SdlHostedSurfaces::send(void* message, bool wait)
{
	auto header = static_cast<mach_msg_header_t*>(message);
	header->msgh_remote_port = _port;
	header->msgh_local_port = MACH_PORT_NULL;
	/* Never blocks the render loop for long: a surface may wait briefly, a frame not at all. A
	 * frame that does not fit in the launcher's queue is dropped; the next one supersedes it. */
	const kern_return_t result =
	    mach_msg(header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, header->msgh_size, 0, MACH_PORT_NULL,
	             wait ? 250 : 0, MACH_PORT_NULL);
	if (result == MACH_SEND_INVALID_DEST)
		_dead = true;
	return result == KERN_SUCCESS;
}

void SdlHostedSurfaces::release(Set& set)
{
	for (int i = 0; i < SDL_LAUNCHER_SURFACE_COUNT; i++)
	{
		if (set.texture[i])
			SDL_DestroyTexture(set.texture[i]);
		if (set.surface[i])
			CFRelease(static_cast<IOSurfaceRef>(set.surface[i]));
		set.texture[i] = nullptr;
		set.surface[i] = nullptr;
	}
	set.width = set.height = 0;
}

bool SdlHostedSurfaces::create(Set& set, SDL_WindowID id, SDL_Renderer* renderer, int width,
                               int height)
{
	release(set);
	set.generation = ++_generations[id];
	set.current = 0;
	@autoreleasepool
	{
		NSDictionary* properties = @{
			(NSString*)kIOSurfaceWidth : @(width),
			(NSString*)kIOSurfaceHeight : @(height),
			(NSString*)kIOSurfaceBytesPerElement : @4,
			(NSString*)kIOSurfacePixelFormat : @((uint32_t)'BGRA')
		};
		for (int i = 0; i < SDL_LAUNCHER_SURFACE_COUNT; i++)
		{
			IOSurfaceRef surface = IOSurfaceCreate((CFDictionaryRef)properties);
			set.surface[i] = surface;
			CVPixelBufferRef buffer = nullptr;
			if (!surface || CVPixelBufferCreateWithIOSurface(kCFAllocatorDefault, surface, nullptr,
			                                                 &buffer) != kCVReturnSuccess)
			{
				release(set);
				return false;
			}
			/* SDL's Metal renderer makes a texture that shares the surface's memory, so drawing
			 * to it is drawing into the surface the launcher shows. */
			const SDL_PropertiesID props = SDL_CreateProperties();
			SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER,
			                      SDL_PIXELFORMAT_ARGB8888);
			SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER,
			                      SDL_TEXTUREACCESS_TARGET);
			SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, width);
			SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, height);
			SDL_SetPointerProperty(props, SDL_PROP_TEXTURE_CREATE_METAL_PIXELBUFFER_POINTER,
			                       buffer);
			set.texture[i] = SDL_CreateTextureWithProperties(renderer, props);
			SDL_DestroyProperties(props);
			CVPixelBufferRelease(buffer);
			if (!set.texture[i])
			{
				release(set);
				return false;
			}
			std::ignore = SDL_SetTextureBlendMode(set.texture[i], SDL_BLENDMODE_NONE);
		}
	}
	set.width = width;
	set.height = height;

	for (int i = 0; i < SDL_LAUNCHER_SURFACE_COUNT; i++)
	{
		SdlLauncherSurfaceMessage message = {};
		message.header.msgh_bits =
		    MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) | MACH_MSGH_BITS_COMPLEX;
		message.header.msgh_size = sizeof(message);
		message.header.msgh_id = SDL_LAUNCHER_MSG_SURFACE;
		message.body.msgh_descriptor_count = 1;
		message.surface.name = IOSurfaceCreateMachPort(static_cast<IOSurfaceRef>(set.surface[i]));
		message.surface.disposition = MACH_MSG_TYPE_COPY_SEND;
		message.surface.type = MACH_MSG_PORT_DESCRIPTOR;
		message.windowId = id;
		message.generation = set.generation;
		message.index = static_cast<uint32_t>(i);
		message.width = static_cast<uint32_t>(width);
		message.height = static_cast<uint32_t>(height);
		const bool sent = send(&message, true);
		mach_port_deallocate(mach_task_self(), message.surface.name);
		if (!sent)
		{
			/* The launcher must have all three before a frame names them: start over later. */
			release(set);
			return false;
		}
	}
	return true;
}

SDL_Texture* SdlHostedSurfaces::target(SDL_Window* window, SDL_Renderer* renderer)
{
	if (!connected() || !window || !renderer)
		return nullptr;
	int width = 0;
	int height = 0;
	if (!SdlHosted::pixelSize(window, &width, &height) || width <= 0 || height <= 0)
		return nullptr;
	const auto id = SDL_GetWindowID(window);
	auto& set = _sets[id];
	if (set.width != width || set.height != height)
	{
		if (!create(set, id, renderer, width, height))
			return nullptr;
	}
	return set.texture[set.current];
}

bool SdlHostedSurfaces::present(SDL_Window* window, SDL_Renderer* renderer)
{
	const auto found = _sets.find(SDL_GetWindowID(window));
	if (found == _sets.end() || found->second.width == 0)
		return false;
	auto& set = found->second;
	/* Leave the surface and hand the drawing to the GPU before the launcher is told. */
	if (!SDL_SetRenderTarget(renderer, nullptr) || !SDL_FlushRenderer(renderer))
		return false;
	SdlLauncherFrameMessage message = {};
	message.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	message.header.msgh_size = sizeof(message);
	message.header.msgh_id = SDL_LAUNCHER_MSG_FRAME;
	message.windowId = found->first;
	message.generation = set.generation;
	message.index = set.current;
	message.sequence = ++set.sequence;
	/* A dropped frame message still counts as presented: the picture is complete, and the
	 * rotation must go on so that the surface the launcher shows is never drawn into. */
	std::ignore = send(&message, false);
	set.current = (set.current + 1) % SDL_LAUNCHER_SURFACE_COUNT;
	return !_dead;
}

void SdlHostedSurfaces::forget(SDL_WindowID id)
{
	const auto found = _sets.find(id);
	if (found == _sets.end())
		return;
	release(found->second);
	_sets.erase(found);
}
