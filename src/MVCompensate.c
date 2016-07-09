// Make a motion compensate temporal denoiser
// Author: Manao
// Copyright(c)2006 A.G.Balakhnin aka Fizick (YUY2, overlap, edges processing)
// See legal notice in Copying.txt for more information

// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA, or visit
// http://www.gnu.org/copyleft/gpl.html .

#include <VapourSynth.h>
#include <VSHelper.h>

#include "CopyCode.h"
#include "Fakery.h"
#include "Overlap.h"
#include "MaskFun.h"
#include "MVAnalysisData.h"


typedef struct MVCompensateData {
    VSNodeRef *node;
    const VSVideoInfo *vi;
    const VSVideoInfo *supervi;

    VSNodeRef *super;
    VSNodeRef *vectors;

    int scBehavior;
    int thSAD;
    int fields;
    int time256;
    int nSCD1;
    int nSCD2;
    int opt;
    int tff;
    int tff_exists;

    MVAnalysisData vectors_data;

    int nSuperHPad;
    int nSuperVPad;
    int nSuperPel;
    int nSuperModeYUV;
    int nSuperLevels;

    int dstTempPitch;
    int dstTempPitchUV;

    OverlapWindows *OverWins;
    OverlapWindows *OverWinsUV;

    OverlapsFunction OVERS[3];
    COPYFunction BLIT[3];
    ToPixelsFunction ToPixels;
} MVCompensateData;


static void VS_CC mvcompensateInit(VSMap *in, VSMap *out, void **instanceData, VSNode *node, VSCore *core, const VSAPI *vsapi) {
    (void)in;
    (void)out;
    (void)core;
    MVCompensateData *d = (MVCompensateData *)*instanceData;
    vsapi->setVideoInfo(d->vi, 1, node);
}


static const VSFrameRef *VS_CC mvcompensateGetFrame(int n, int activationReason, void **instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    (void)frameData;

    MVCompensateData *d = (MVCompensateData *)*instanceData;

    if (activationReason == arInitial) {
        // XXX off could be calculated during initialisation
        int off, nref;
        if (d->vectors_data.nDeltaFrame > 0) {
            off = d->vectors_data.isBackward ? 1 : -1;
            off *= d->vectors_data.nDeltaFrame;
            nref = n + off;
        } else {
            nref = -d->vectors_data.nDeltaFrame; // positive frame number (special static mode)
        }

        vsapi->requestFrameFilter(n, d->vectors, frameCtx);

        if (nref < n && nref >= 0)
            vsapi->requestFrameFilter(nref, d->super, frameCtx);

        vsapi->requestFrameFilter(n, d->super, frameCtx);

        if (nref >= n && nref < d->vi->numFrames)
            vsapi->requestFrameFilter(nref, d->super, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        const VSFrameRef *src = vsapi->getFrameFilter(n, d->super, frameCtx);
        VSFrameRef *dst = vsapi->newVideoFrame(d->vi->format, d->vi->width, d->vi->height, src, core);


        uint8_t *pDst[3] = { NULL };
        uint8_t *pDstCur[3] = { NULL };
        const uint8_t *pRef[3] = { NULL };
        int nDstPitches[3] = { 0 };
        int nRefPitches[3] = { 0 };
        const uint8_t *pSrc[3] = { NULL };
        int nSrcPitches[3] = { 0 };

        const VSFrameRef *mvn = vsapi->getFrameFilter(n, d->vectors, frameCtx);
        FakeGroupOfPlanes fgop;
        fgopInit(&fgop, &d->vectors_data);
        const VSMap *mvprops = vsapi->getFramePropsRO(mvn);
        fgopUpdate(&fgop, (const int *)vsapi->propGetData(mvprops, prop_MVTools_vectors, 0, NULL));
        vsapi->freeFrame(mvn);

        int off, nref;
        if (d->vectors_data.nDeltaFrame > 0) {
            off = (d->vectors_data.isBackward) ? 1 : -1;
            off *= d->vectors_data.nDeltaFrame;
            nref = n + off;
        } else {
            nref = -d->vectors_data.nDeltaFrame; // positive frame number (special static mode)
        }


        const int xRatioUV = d->vectors_data.xRatioUV;
        const int yRatioUV = d->vectors_data.yRatioUV;
        const int ySubUV = (yRatioUV == 2) ? 1 : 0;
        const int xSubUV = (xRatioUV == 2) ? 1 : 0;
        const int nWidth[3] = { d->vectors_data.nWidth, nWidth[0] >> xSubUV, nWidth[1] };
        const int nHeight[3] = { d->vectors_data.nHeight, nHeight[0] >> ySubUV, nHeight[1] };
        const int nOverlapX[3] = { d->vectors_data.nOverlapX, nOverlapX[0] >> xSubUV, nOverlapX[1] };
        const int nOverlapY[3] = { d->vectors_data.nOverlapY, nOverlapY[0] >> ySubUV, nOverlapY[1] };
        const int nBlkSizeX[3] = { d->vectors_data.nBlkSizeX, nBlkSizeX[0] >> xSubUV, nBlkSizeX[1] };
        const int nBlkSizeY[3] = { d->vectors_data.nBlkSizeY, nBlkSizeY[0] >> ySubUV, nBlkSizeY[1] };
        const int nBlkX = d->vectors_data.nBlkX;
        const int nBlkY = d->vectors_data.nBlkY;
        const int opt = d->opt;
        const int thSAD = d->thSAD;
        const int dstTempPitch[3] = { d->dstTempPitch, d->dstTempPitchUV, d->dstTempPitchUV };
        const int nSuperModeYUV = d->nSuperModeYUV;
        const int nPel = d->vectors_data.nPel;
        const int nHPadding[3] = { d->vectors_data.nHPadding, nHPadding[0] >> xSubUV, nHPadding[1] };
        const int nVPadding[3] = { d->vectors_data.nVPadding, nVPadding[0] >> ySubUV, nVPadding[1] };
        const int scBehavior = d->scBehavior;
        const int fields = d->fields;
        const int time256 = d->time256;

        int bitsPerSample = d->supervi->format->bitsPerSample;
        int bytesPerSample = d->supervi->format->bytesPerSample;


        int nWidth_B[3] = { nBlkX * (nBlkSizeX[0] - nOverlapX[0]) + nOverlapX[0], nWidth_B[0] >> xSubUV, nWidth_B[1] };
        int nHeight_B[3] = { nBlkY * (nBlkSizeY[0] - nOverlapY[0]) + nOverlapY[0], nHeight_B[0] >> ySubUV, nHeight_B[1] };


        int num_planes = 1;
        if (nSuperModeYUV & UVPLANES)
            num_planes = 3;

        if (fgopIsUsable(&fgop, d->nSCD1, d->nSCD2)) {
            // No need to check nref because nref is always in range when fgop is usable.
            const VSFrameRef *ref = vsapi->getFrameFilter(nref, d->super, frameCtx);
            for (int i = 0; i < d->supervi->format->numPlanes; i++) {
                pDstCur[i] = pDst[i] = vsapi->getWritePtr(dst, i);
                nDstPitches[i] = vsapi->getStride(dst, i);
                pSrc[i] = vsapi->getReadPtr(src, i);
                nSrcPitches[i] = vsapi->getStride(src, i);
                pRef[i] = vsapi->getReadPtr(ref, i);
                nRefPitches[i] = vsapi->getStride(ref, i);
            }

            MVGroupOfFrames pRefGOF, pSrcGOF;

            mvgofInit(&pRefGOF, d->nSuperLevels, nWidth[0], nHeight[0], d->nSuperPel, d->nSuperHPad, d->nSuperVPad, nSuperModeYUV, opt, xRatioUV, yRatioUV, bitsPerSample);
            mvgofInit(&pSrcGOF, d->nSuperLevels, nWidth[0], nHeight[0], d->nSuperPel, d->nSuperHPad, d->nSuperVPad, nSuperModeYUV, opt, xRatioUV, yRatioUV, bitsPerSample);

            mvgofUpdate(&pRefGOF, (uint8_t **)pRef, nRefPitches);
            mvgofUpdate(&pSrcGOF, (uint8_t **)pSrc, nSrcPitches);


            MVPlane **pRefPlanes = pRefGOF.frames[0]->planes;
            MVPlane **pSrcPlanes = pSrcGOF.frames[0]->planes;


            int fieldShift = 0;
            if (fields && nPel > 1 && ((nref - n) % 2 != 0)) {
                int err;
                const VSMap *props = vsapi->getFramePropsRO(src);
                int src_top_field = !!vsapi->propGetInt(props, "_Field", 0, &err);
                if (err && !d->tff_exists) {
                    vsapi->setFilterError("Compensate: _Field property not found in input frame. Therefore, you must pass tff argument.", frameCtx);
                    fgopDeinit(&fgop);
                    mvgofDeinit(&pRefGOF);
                    mvgofDeinit(&pSrcGOF);
                    vsapi->freeFrame(src);
                    vsapi->freeFrame(dst);
                    vsapi->freeFrame(ref);
                    return NULL;
                }

                if (d->tff_exists)
                    src_top_field = d->tff ^ (n % 2);

                props = vsapi->getFramePropsRO(ref);
                int ref_top_field = !!vsapi->propGetInt(props, "_Field", 0, &err);
                if (err && !d->tff_exists) {
                    vsapi->setFilterError("Compensate: _Field property not found in input frame. Therefore, you must pass tff argument.", frameCtx);
                    fgopDeinit(&fgop);
                    mvgofDeinit(&pRefGOF);
                    mvgofDeinit(&pSrcGOF);
                    vsapi->freeFrame(src);
                    vsapi->freeFrame(dst);
                    vsapi->freeFrame(ref);
                    return NULL;
                }

                if (d->tff_exists)
                    ref_top_field = d->tff ^ (nref % 2);

                fieldShift = (src_top_field && !ref_top_field) ? nPel / 2 : ((ref_top_field && !src_top_field) ? -(nPel / 2) : 0);
                // vertical shift of fields for fieldbased video at finest level pel2
            }

            if (nOverlapX[0] == 0 && nOverlapY[0] == 0) {
                for (int by = 0; by < nBlkY; by++) {
                    int xx[3] = { 0 };

                    for (int bx = 0; bx < nBlkX; bx++) {
                        int i = by * nBlkX + bx;
                        const FakeBlockData *block = fgopGetBlock(&fgop, 0, i);

                        int blx[3], bly[3];
                        MVPlane **pPlanes;

                        if (block->vector.sad < thSAD) {
                            blx[0] = block->x * nPel + block->vector.x * time256 / 256;
                            bly[0] = block->y * nPel + block->vector.y * time256 / 256 + fieldShift;
                            pPlanes = pRefPlanes;
                        } else {
                            blx[0] = bx * nBlkSizeX[0] * nPel;
                            bly[0] = by * nBlkSizeY[0] * nPel + fieldShift;
                            pPlanes = pSrcPlanes;
                        }
                        blx[1] = blx[2] = blx[0] >> xSubUV;
                        bly[1] = bly[2] = bly[0] >> ySubUV;

                        for (int plane = 0; plane < num_planes; plane++) {
                            d->BLIT[plane](pDstCur[plane] + xx[plane], nDstPitches[plane], mvpGetPointer(pPlanes[plane], blx[plane], bly[plane]), pPlanes[plane]->nPitch);

                            xx[plane] += nBlkSizeX[plane] * bytesPerSample;
                        }
                    }

                    for (int plane = 0; plane < num_planes; plane++)
                        pDstCur[plane] += nBlkSizeY[plane] * nDstPitches[plane];
                }
            } else { // overlap
                uint8_t *DstTemp[3] = { NULL };
                uint8_t *pDstTemp[3] = { NULL };
                for (int plane = 0; plane < num_planes; plane++) {
                    pDstTemp[plane] = DstTemp[plane] = (uint8_t *)malloc(nHeight[plane] * dstTempPitch[plane]);
                    memset(DstTemp[plane], 0, nHeight_B[plane] * dstTempPitch[plane]);
                }

                for (int by = 0; by < nBlkY; by++) {
                    int wby = ((by + nBlkY - 3) / (nBlkY - 2)) * 3;
                    int xx[3] = { 0 };

                    for (int bx = 0; bx < nBlkX; bx++) {
                        // select window
                        int wbx = (bx + nBlkX - 3) / (nBlkX - 2);

                        int16_t *winOver[3] = { overGetWindow(d->OverWins, wby + wbx) };
                        if (nSuperModeYUV & UVPLANES)
                            winOver[1] = winOver[2] = overGetWindow(d->OverWinsUV, wby + wbx);

                        int i = by * nBlkX + bx;
                        const FakeBlockData *block = fgopGetBlock(&fgop, 0, i);

                        int blx[3], bly[3];
                        MVPlane **pPlanes;

                        if (block->vector.sad < thSAD) {
                            blx[0] = block->x * nPel + block->vector.x * time256 / 256;
                            bly[0] = block->y * nPel + block->vector.y * time256 / 256 + fieldShift;
                            pPlanes = pRefPlanes;
                        } else {
                            blx[0] = bx * (nBlkSizeX[0] - nOverlapX[0]) * nPel;
                            bly[0] = by * (nBlkSizeY[0] - nOverlapY[0]) * nPel + fieldShift;
                            pPlanes = pSrcPlanes;
                        }
                        blx[1] = blx[2] = blx[0] >> xSubUV;
                        bly[1] = bly[2] = bly[0] >> ySubUV;

                        for (int plane = 0; plane < num_planes; plane++) {
                            d->OVERS[plane](pDstTemp[plane] + xx[plane] * 2, dstTempPitch[plane], mvpGetPointer(pPlanes[plane], blx[plane], bly[plane]), pPlanes[plane]->nPitch, winOver[plane], nBlkSizeX[plane]);

                            xx[plane] += (nBlkSizeX[plane] - nOverlapX[plane]) * bytesPerSample;
                        }
                    }

                    for (int plane = 0; plane < num_planes; plane++) {
                        pDstTemp[plane] += dstTempPitch[plane] * (nBlkSizeY[plane] - nOverlapY[plane]);
                        pDstCur[plane] += nDstPitches[plane] * (nBlkSizeY[plane] - nOverlapY[plane]);
                    }
                }

                for (int plane = 0; plane < num_planes; plane++) {
                    d->ToPixels(pDst[plane], nDstPitches[plane], DstTemp[plane], dstTempPitch[plane], nWidth_B[plane], nHeight_B[plane], bitsPerSample);

                    free(DstTemp[plane]);
                }
            }

            const uint8_t **scSrc;
            int *scPitches;

            if (scBehavior) {
                scSrc = pSrc;
                scPitches = nSrcPitches;
            } else {
                scSrc = pRef;
                scPitches = nRefPitches;
            }

            for (int plane = 0; plane < num_planes; plane++) {
                if (nWidth_B[0] < nWidth[0]) { // padding of right non-covered region
                    vs_bitblt(pDst[plane] + nWidth_B[plane] * bytesPerSample, nDstPitches[plane],
                              scSrc[plane] + (nWidth_B[plane] + nHPadding[plane]) * bytesPerSample + nVPadding[plane] * scPitches[plane], scPitches[plane],
                              (nWidth[plane] - nWidth_B[plane]) * bytesPerSample, nHeight_B[plane]);
                }

                if (nHeight_B[0] < nHeight[0]) { // padding of bottom non-covered region
                    vs_bitblt(pDst[plane] + nHeight_B[plane] * nDstPitches[plane], nDstPitches[plane],
                              scSrc[plane] + nHPadding[plane] * bytesPerSample + (nHeight_B[plane] + nVPadding[plane]) * scPitches[plane], scPitches[plane],
                              nWidth[plane] * bytesPerSample, nHeight[plane] - nHeight_B[plane]);
                }
            }

            mvgofDeinit(&pRefGOF);
            mvgofDeinit(&pSrcGOF);

            vsapi->freeFrame(ref);
        } else { // fgopIsUsable()
            if (!scBehavior && nref < d->vi->numFrames && nref >= 0) {
                vsapi->freeFrame(src);
                src = vsapi->getFrameFilter(nref, d->super, frameCtx);
            }

            for (int plane = 0; plane < num_planes; plane++) {
                pDst[plane] = vsapi->getWritePtr(dst, plane);
                nDstPitches[plane] = vsapi->getStride(dst, plane);
                pSrc[plane] = vsapi->getReadPtr(src, plane);
                nSrcPitches[plane] = vsapi->getStride(src, plane);

                int nOffset = nHPadding[plane] * bytesPerSample + nVPadding[plane] * nSrcPitches[plane];

                vs_bitblt(pDst[plane], nDstPitches[plane], pSrc[plane] + nOffset, nSrcPitches[plane], nWidth[plane] * bytesPerSample, nHeight[plane]);
            }
        }

        fgopDeinit(&fgop);

        vsapi->freeFrame(src);

        return dst;
    }

    return 0;
}


static void VS_CC mvcompensateFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    (void)core;

    MVCompensateData *d = (MVCompensateData *)instanceData;

    if (d->vectors_data.nOverlapX || d->vectors_data.nOverlapY) {
        overDeinit(d->OverWins);
        free(d->OverWins);
        if (d->nSuperModeYUV & UVPLANES) {
            overDeinit(d->OverWinsUV);
            free(d->OverWinsUV);
        }
    }

    vsapi->freeNode(d->super);
    vsapi->freeNode(d->vectors);
    vsapi->freeNode(d->node);
    free(d);
}


static void selectFunctions(MVCompensateData *d) {
    const int xRatioUV = d->vectors_data.xRatioUV;
    const int yRatioUV = d->vectors_data.yRatioUV;
    const int nBlkSizeX = d->vectors_data.nBlkSizeX;
    const int nBlkSizeY = d->vectors_data.nBlkSizeY;

    OverlapsFunction overs[33][33];
    COPYFunction copys[33][33];

    if (d->vi->format->bitsPerSample == 8) {
        overs[2][2] = mvtools_overlaps_2x2_uint16_t_uint8_t_c;
        copys[2][2] = mvtools_copy_2x2_u8_c;

        overs[2][4] = mvtools_overlaps_2x4_uint16_t_uint8_t_c;
        copys[2][4] = mvtools_copy_2x4_u8_c;

        overs[4][2] = mvtools_overlaps_4x2_uint16_t_uint8_t_c;
        copys[4][2] = mvtools_copy_4x2_u8_c;

        overs[4][4] = mvtools_overlaps_4x4_uint16_t_uint8_t_c;
        copys[4][4] = mvtools_copy_4x4_u8_c;

        overs[4][8] = mvtools_overlaps_4x8_uint16_t_uint8_t_c;
        copys[4][8] = mvtools_copy_4x8_u8_c;

        overs[8][1] = mvtools_overlaps_8x1_uint16_t_uint8_t_c;
        copys[8][1] = mvtools_copy_8x1_u8_c;

        overs[8][2] = mvtools_overlaps_8x2_uint16_t_uint8_t_c;
        copys[8][2] = mvtools_copy_8x2_u8_c;

        overs[8][4] = mvtools_overlaps_8x4_uint16_t_uint8_t_c;
        copys[8][4] = mvtools_copy_8x4_u8_c;

        overs[8][8] = mvtools_overlaps_8x8_uint16_t_uint8_t_c;
        copys[8][8] = mvtools_copy_8x8_u8_c;

        overs[8][16] = mvtools_overlaps_8x16_uint16_t_uint8_t_c;
        copys[8][16] = mvtools_copy_8x16_u8_c;

        overs[16][1] = mvtools_overlaps_16x1_uint16_t_uint8_t_c;
        copys[16][1] = mvtools_copy_16x1_u8_c;

        overs[16][2] = mvtools_overlaps_16x2_uint16_t_uint8_t_c;
        copys[16][2] = mvtools_copy_16x2_u8_c;

        overs[16][4] = mvtools_overlaps_16x4_uint16_t_uint8_t_c;
        copys[16][4] = mvtools_copy_16x4_u8_c;

        overs[16][8] = mvtools_overlaps_16x8_uint16_t_uint8_t_c;
        copys[16][8] = mvtools_copy_16x8_u8_c;

        overs[16][16] = mvtools_overlaps_16x16_uint16_t_uint8_t_c;
        copys[16][16] = mvtools_copy_16x16_u8_c;

        overs[16][32] = mvtools_overlaps_16x32_uint16_t_uint8_t_c;
        copys[16][32] = mvtools_copy_16x32_u8_c;

        overs[32][8] = mvtools_overlaps_32x8_uint16_t_uint8_t_c;
        copys[32][8] = mvtools_copy_32x8_u8_c;

        overs[32][16] = mvtools_overlaps_32x16_uint16_t_uint8_t_c;
        copys[32][16] = mvtools_copy_32x16_u8_c;

        overs[32][32] = mvtools_overlaps_32x32_uint16_t_uint8_t_c;
        copys[32][32] = mvtools_copy_32x32_u8_c;

        d->ToPixels = ToPixels_uint16_t_uint8_t;

        if (d->opt) {
#if defined(MVTOOLS_X86)
            overs[4][2] = mvtools_overlaps_4x2_sse2;
            overs[4][4] = mvtools_overlaps_4x4_sse2;
            overs[4][8] = mvtools_overlaps_4x8_sse2;
            overs[8][1] = mvtools_overlaps_8x1_sse2;
            overs[8][2] = mvtools_overlaps_8x2_sse2;
            overs[8][4] = mvtools_overlaps_8x4_sse2;
            overs[8][8] = mvtools_overlaps_8x8_sse2;
            overs[8][16] = mvtools_overlaps_8x16_sse2;
            overs[16][1] = mvtools_overlaps_16x1_sse2;
            overs[16][2] = mvtools_overlaps_16x2_sse2;
            overs[16][4] = mvtools_overlaps_16x4_sse2;
            overs[16][8] = mvtools_overlaps_16x8_sse2;
            overs[16][16] = mvtools_overlaps_16x16_sse2;
            overs[16][32] = mvtools_overlaps_16x32_sse2;
            overs[32][8] = mvtools_overlaps_32x8_sse2;
            overs[32][16] = mvtools_overlaps_32x16_sse2;
            overs[32][32] = mvtools_overlaps_32x32_sse2;
#endif
        }
    } else {
        overs[2][2] = mvtools_overlaps_2x2_uint32_t_uint16_t_c;
        copys[2][2] = mvtools_copy_2x2_u16_c;

        overs[2][4] = mvtools_overlaps_2x4_uint32_t_uint16_t_c;
        copys[2][4] = mvtools_copy_2x4_u16_c;

        overs[4][2] = mvtools_overlaps_4x2_uint32_t_uint16_t_c;
        copys[4][2] = mvtools_copy_4x2_u16_c;

        overs[4][4] = mvtools_overlaps_4x4_uint32_t_uint16_t_c;
        copys[4][4] = mvtools_copy_4x4_u16_c;

        overs[4][8] = mvtools_overlaps_4x8_uint32_t_uint16_t_c;
        copys[4][8] = mvtools_copy_4x8_u16_c;

        overs[8][1] = mvtools_overlaps_8x1_uint32_t_uint16_t_c;
        copys[8][1] = mvtools_copy_8x1_u16_c;

        overs[8][2] = mvtools_overlaps_8x2_uint32_t_uint16_t_c;
        copys[8][2] = mvtools_copy_8x2_u16_c;

        overs[8][4] = mvtools_overlaps_8x4_uint32_t_uint16_t_c;
        copys[8][4] = mvtools_copy_8x4_u16_c;

        overs[8][8] = mvtools_overlaps_8x8_uint32_t_uint16_t_c;
        copys[8][8] = mvtools_copy_8x8_u16_c;

        overs[8][16] = mvtools_overlaps_8x16_uint32_t_uint16_t_c;
        copys[8][16] = mvtools_copy_8x16_u16_c;

        overs[16][1] = mvtools_overlaps_16x1_uint32_t_uint16_t_c;
        copys[16][1] = mvtools_copy_16x1_u16_c;

        overs[16][2] = mvtools_overlaps_16x2_uint32_t_uint16_t_c;
        copys[16][2] = mvtools_copy_16x2_u16_c;

        overs[16][4] = mvtools_overlaps_16x4_uint32_t_uint16_t_c;
        copys[16][4] = mvtools_copy_16x4_u16_c;

        overs[16][8] = mvtools_overlaps_16x8_uint32_t_uint16_t_c;
        copys[16][8] = mvtools_copy_16x8_u16_c;

        overs[16][16] = mvtools_overlaps_16x16_uint32_t_uint16_t_c;
        copys[16][16] = mvtools_copy_16x16_u16_c;

        overs[16][32] = mvtools_overlaps_16x32_uint32_t_uint16_t_c;
        copys[16][32] = mvtools_copy_16x32_u16_c;

        overs[32][8] = mvtools_overlaps_32x8_uint32_t_uint16_t_c;
        copys[32][8] = mvtools_copy_32x8_u16_c;

        overs[32][16] = mvtools_overlaps_32x16_uint32_t_uint16_t_c;
        copys[32][16] = mvtools_copy_32x16_u16_c;

        overs[32][32] = mvtools_overlaps_32x32_uint32_t_uint16_t_c;
        copys[32][32] = mvtools_copy_32x32_u16_c;

        d->ToPixels = ToPixels_uint32_t_uint16_t;
    }

    d->OVERS[0] = overs[nBlkSizeX][nBlkSizeY];
    d->BLIT[0] = copys[nBlkSizeX][nBlkSizeY];

    d->OVERS[1] = d->OVERS[2] = overs[nBlkSizeX / xRatioUV][nBlkSizeY / yRatioUV];
    d->BLIT[1] = d->BLIT[2] = copys[nBlkSizeX / xRatioUV][nBlkSizeY / yRatioUV];
}


static void VS_CC mvcompensateCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    (void)userData;

    MVCompensateData d;
    MVCompensateData *data;

    int err;

    d.scBehavior = !!vsapi->propGetInt(in, "scbehavior", 0, &err);
    if (err)
        d.scBehavior = 1;

    d.thSAD = int64ToIntS(vsapi->propGetInt(in, "thsad", 0, &err));
    if (err)
        d.thSAD = 10000;

    d.fields = !!vsapi->propGetInt(in, "fields", 0, &err);

    double time = vsapi->propGetFloat(in, "time", 0, &err);
    if (err)
        time = 100.0;

    d.nSCD1 = int64ToIntS(vsapi->propGetInt(in, "thscd1", 0, &err));
    if (err)
        d.nSCD1 = MV_DEFAULT_SCD1;

    d.nSCD2 = int64ToIntS(vsapi->propGetInt(in, "thscd2", 0, &err));
    if (err)
        d.nSCD2 = MV_DEFAULT_SCD2;

    d.opt = !!vsapi->propGetInt(in, "opt", 0, &err);
    if (err)
        d.opt = 1;

    d.tff = !!vsapi->propGetInt(in, "tff", 0, &err);
    d.tff_exists = !err;


    if (time < 0.0 || time > 100.0) {
        vsapi->setError(out, "Compensate: time must be between 0.0 and 100.0 (inclusive).");
        return;
    }


    d.super = vsapi->propGetNode(in, "super", 0, NULL);

#define ERROR_SIZE 1024
    char errorMsg[ERROR_SIZE] = "Compensate: failed to retrieve first frame from super clip. Error message: ";
    size_t errorLen = strlen(errorMsg);
    const VSFrameRef *evil = vsapi->getFrame(0, d.super, errorMsg + errorLen, ERROR_SIZE - errorLen);
#undef ERROR_SIZE
    if (!evil) {
        vsapi->setError(out, errorMsg);
        vsapi->freeNode(d.super);
        return;
    }
    const VSMap *props = vsapi->getFramePropsRO(evil);
    int evil_err[6];
    int nHeightS = int64ToIntS(vsapi->propGetInt(props, "Super_height", 0, &evil_err[0]));
    d.nSuperHPad = int64ToIntS(vsapi->propGetInt(props, "Super_hpad", 0, &evil_err[1]));
    d.nSuperVPad = int64ToIntS(vsapi->propGetInt(props, "Super_vpad", 0, &evil_err[2]));
    d.nSuperPel = int64ToIntS(vsapi->propGetInt(props, "Super_pel", 0, &evil_err[3]));
    d.nSuperModeYUV = int64ToIntS(vsapi->propGetInt(props, "Super_modeyuv", 0, &evil_err[4]));
    d.nSuperLevels = int64ToIntS(vsapi->propGetInt(props, "Super_levels", 0, &evil_err[5]));
    vsapi->freeFrame(evil);

    for (int i = 0; i < 6; i++)
        if (evil_err[i]) {
            vsapi->setError(out, "Compensate: required properties not found in first frame of super clip. Maybe clip didn't come from mv.Super? Was the first frame trimmed away?");
            vsapi->freeNode(d.super);
            return;
        }


    d.vectors = vsapi->propGetNode(in, "vectors", 0, NULL);

#define ERROR_SIZE 512
    char error[ERROR_SIZE + 1] = { 0 };
    const char *filter_name = "Compensate";

    adataFromVectorClip(&d.vectors_data, d.vectors, filter_name, "vectors", vsapi, error, ERROR_SIZE);

    int nSCD1_old = d.nSCD1;
    scaleThSCD(&d.nSCD1, &d.nSCD2, &d.vectors_data, filter_name, error, ERROR_SIZE);
#undef ERROR_SIZE

    if (error[0]) {
        vsapi->setError(out, error);

        vsapi->freeNode(d.super);
        vsapi->freeNode(d.vectors);
        return;
    }


    if (d.fields && d.vectors_data.nPel < 2) {
        vsapi->setError(out, "Compensate: fields option requires pel > 1.");
        vsapi->freeNode(d.super);
        vsapi->freeNode(d.vectors);
        return;
    }

    d.thSAD = (int64_t)d.thSAD * d.nSCD1 / nSCD1_old; // normalize to block SAD


    d.node = vsapi->propGetNode(in, "clip", 0, 0);
    d.vi = vsapi->getVideoInfo(d.node);


    d.dstTempPitch = ((d.vectors_data.nWidth + 15) / 16) * 16 * d.vi->format->bytesPerSample * 2;
    d.dstTempPitchUV = (((d.vectors_data.nWidth / d.vectors_data.xRatioUV) + 15) / 16) * 16 * d.vi->format->bytesPerSample * 2;


    d.supervi = vsapi->getVideoInfo(d.super);
    int nSuperWidth = d.supervi->width;

    if (d.vectors_data.nHeight != nHeightS || d.vectors_data.nHeight != d.vi->height || d.vectors_data.nWidth != nSuperWidth - d.nSuperHPad * 2 || d.vectors_data.nWidth != d.vi->width || d.vectors_data.nPel != d.nSuperPel) {
        vsapi->setError(out, "Compensate: wrong source or super clip frame size.");
        vsapi->freeNode(d.super);
        vsapi->freeNode(d.vectors);
        vsapi->freeNode(d.node);
        return;
    }

    if (!isConstantFormat(d.vi) || d.vi->format->bitsPerSample > 16 || d.vi->format->sampleType != stInteger || d.vi->format->subSamplingW > 1 || d.vi->format->subSamplingH > 1 || (d.vi->format->colorFamily != cmYUV && d.vi->format->colorFamily != cmGray)) {
        vsapi->setError(out, "Compensate: input clip must be GRAY, 420, 422, 440, or 444, up to 16 bits, with constant dimensions.");
        vsapi->freeNode(d.super);
        vsapi->freeNode(d.vectors);
        vsapi->freeNode(d.node);
        return;
    }

    if (d.vi->format->bitsPerSample > 8)
        d.opt = 0;

    if (d.vectors_data.nOverlapX || d.vectors_data.nOverlapY) {
        d.OverWins = (OverlapWindows *)malloc(sizeof(OverlapWindows));
        overInit(d.OverWins, d.vectors_data.nBlkSizeX, d.vectors_data.nBlkSizeY, d.vectors_data.nOverlapX, d.vectors_data.nOverlapY);
        if (d.nSuperModeYUV & UVPLANES) {
            d.OverWinsUV = (OverlapWindows *)malloc(sizeof(OverlapWindows));
            overInit(d.OverWinsUV, d.vectors_data.nBlkSizeX / d.vectors_data.xRatioUV, d.vectors_data.nBlkSizeY / d.vectors_data.yRatioUV, d.vectors_data.nOverlapX / d.vectors_data.xRatioUV, d.vectors_data.nOverlapY / d.vectors_data.yRatioUV);
        }
    }

    d.time256 = (int)(time * 256 / 100);

    selectFunctions(&d);


    data = (MVCompensateData *)malloc(sizeof(d));
    *data = d;

    vsapi->createFilter(in, out, "Compensate", mvcompensateInit, mvcompensateGetFrame, mvcompensateFree, fmParallel, 0, data, core);
}


void mvcompensateRegister(VSRegisterFunction registerFunc, VSPlugin *plugin) {
    registerFunc("Compensate",
                 "clip:clip;"
                 "super:clip;"
                 "vectors:clip;"
                 "scbehavior:int:opt;"
                 "thsad:int:opt;"
                 "fields:int:opt;"
                 "time:float:opt;"
                 "thscd1:int:opt;"
                 "thscd2:int:opt;"
                 "opt:int:opt;"
                 "tff:int:opt;",
                 mvcompensateCreate, 0, plugin);
}
