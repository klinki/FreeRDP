/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * YUV threadpool slot reuse tests
 *
 * Copyright 2026 FreeRDP contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdlib.h>
#include <string.h>

#include <winpr/wtypes.h>
#include <freerdp/codec/color.h>
#include <freerdp/codec/yuv.h>
#include <freerdp/primitives.h>

#define TEST_W 1024
#define TEST_H 1024

static void fill_plane(BYTE* plane, UINT32 stride, UINT32 w, UINT32 h, int seed)
{
	for (UINT32 y = 0; y < h; y++)
	{
		for (UINT32 x = 0; x < w; x++)
			plane[y * stride + x] = (BYTE)((x + y + seed) & 0xff);
	}
}

static void read_stats(UINT32* tiles, UINT32* created, UINT32* reused)
{
	UINT32 t = 0;
	UINT32 c = 0;
	UINT32 r = 0;
	yuv_pool_stats(&t, &c, &r);
	if (tiles)
		*tiles = t;
	if (created)
		*created = c;
	if (reused)
		*reused = r;
}

int TestFreeRDPCodecYUV(WINPR_ATTR_UNUSED int argc, WINPR_ATTR_UNUSED char* argv[])
{
	const UINT32 stride = TEST_W;
	const UINT32 dstStep = TEST_W * 4;
	const RECTANGLE_16 region = { 0, 0, TEST_W, TEST_H };
	int rc = -1;

	BYTE* src[3] = { NULL, NULL, NULL };
	BYTE* dstPlanes[3] = { NULL, NULL, NULL };
	BYTE* dest1 = NULL;
	BYTE* dest2 = NULL;

	for (int i = 0; i < 3; i++)
	{
		src[i] = calloc(1, stride * TEST_H);
		dstPlanes[i] = calloc(1, stride * TEST_H);
		if (!src[i] || !dstPlanes[i])
			goto out;
		fill_plane(src[i], stride, TEST_W, TEST_H, i * 64);
	}
	dest1 = calloc(1, dstStep * TEST_H);
	dest2 = calloc(1, dstStep * TEST_H);
	if (!dest1 || !dest2)
		goto out;

	YUV_CONTEXT* yuv = yuv_context_new(FALSE, 0);
	if (!yuv)
		goto out;
	if (!yuv_context_reset(yuv, TEST_W, TEST_H))
		goto free_ctx;

	const BYTE* csrc[3] = { src[0], src[1], src[2] };
	const UINT32 istride[3] = { stride, stride, stride };
	const UINT32 ostride[3] = { stride, stride, stride };

	/* First decode warms the slots; the combine pass and the process pass
	 * share nothing, so each path binds its own slots. */
	if (!yuv444_context_decode(yuv, AVC444_LUMA, csrc, istride, TEST_H, dstPlanes, ostride,
	                           PIXEL_FORMAT_BGRA32, dest1, dstStep, &region, 1))
		goto free_ctx;

	UINT32 tiles1 = 0;
	UINT32 created1 = 0;
	UINT32 reused1 = 0;
	read_stats(&tiles1, &created1, &reused1);

	/* Second identical decode must reproduce every pixel: reuse must not
	 * change results. */
	if (!yuv444_context_decode(yuv, AVC444_LUMA, csrc, istride, TEST_H, dstPlanes, ostride,
	                           PIXEL_FORMAT_BGRA32, dest2, dstStep, &region, 1))
		goto free_ctx;
	if (memcmp(dest1, dest2, dstStep * TEST_H) != 0)
		goto free_ctx;

	UINT32 tiles2 = 0;
	UINT32 created2 = 0;
	UINT32 reused2 = 0;
	read_stats(&tiles2, &created2, &reused2);

	if (tiles1 > 0)
	{
		/* Threaded path: the steady state creates nothing; every tile of
		 * the second run must come from reused slots. */
		if (tiles2 <= tiles1)
			goto free_ctx;
		if ((created2 - created1) != 0)
			goto free_ctx;
		if ((reused2 - reused1) == 0)
			goto free_ctx;
	}
	/* Single-CPU fallback (no threads): tiles stay zero and output above
	 * already proved the direct path. */

	/* A different callback on shared process slots (420 vs 444) and a
	 * reset must both keep working. */
	if (!yuv420_context_decode(yuv, csrc, istride, TEST_H, PIXEL_FORMAT_BGRA32, dest1, dstStep,
	                           &region, 1))
		goto free_ctx;
	if (!yuv_context_reset(yuv, TEST_W, TEST_H))
		goto free_ctx;
	if (!yuv444_context_decode(yuv, AVC444_LUMA, csrc, istride, TEST_H, dstPlanes, ostride,
	                           PIXEL_FORMAT_BGRA32, dest1, dstStep, &region, 1))
		goto free_ctx;

	rc = 0;
free_ctx:
	yuv_context_free(yuv);
out:
	for (int i = 0; i < 3; i++)
	{
		free(src[i]);
		free(dstPlanes[i]);
	}
	free(dest1);
	free(dest2);
	return rc;
}
