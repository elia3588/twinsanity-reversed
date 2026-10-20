#include "mpeg.h"

// libmpeg's motion compensation: a macroblock's prediction made from blocks of the reference pictures, their macroblocks fetched
// into the scratchpad by the toSPR channel and read by hand-written MMI routines into 16 bit pixels, then stored into the picture
// with the IPU's blocks added

extern "C"
{
    extern const char g_MpegInvalidMotionType0[] RETAIL(D_00307600);
    extern const char g_MpegInvalidMotionType1[] RETAIL(D_00307620);
    extern const char g_MpegInvalidMotionType2[] RETAIL(D_00307640);
    extern const char g_MpegMotionTypeIgnored[] RETAIL(D_00307660);
    extern const char g_MpegIntraWithoutBlocks[] RETAIL(D_00307688);
    // The prediction routines: the luma's 8, then the chroma's
    extern MpegPredictionRoutine g_MpegPredictionRoutines[16] RETAIL(jtbl_002E8238);
    // The toSPR channel's chain fetching the references' macroblocks: two tags a reference
    extern u64 g_MpegReferenceChain[0x40] RETAIL(D_003C6F80);
}

namespace Libmpeg
{
// The prediction routines, in software (defined at the end of this file)
extern MpegPredictionRoutine g_SoftwarePredictionRoutines[16];

namespace
{
constexpr s32 ChromaRoutines = 8;
// A reference's macroblocks in the scratchpad: the column of two its block starts in, then the column to its right
constexpr u32 ColumnQuadwords = 2 * MacroblockQuadwords;
constexpr u32 ColumnBytes = ColumnQuadwords * 16;
// A macroblock's luma: 16 rows of 16 pixels (a byte each), then its chroma's Cb and Cr: 8 rows of 8 each
constexpr s32 LumaShift = 4;
constexpr s32 LumaRows = 1 << LumaShift;
constexpr s32 LumaRowBytes = 1 << LumaShift;
constexpr s32 LumaBytes = LumaRows * LumaRowBytes;
constexpr s32 ChromaShift = 3;
constexpr s32 ChromaRows = 1 << ChromaShift;
constexpr s32 ChromaRowBytes = 1 << ChromaShift;
// The prediction: the luma's rows of 16 bit pixels (32 bytes), then the chroma's (16 bytes)
constexpr s32 PredictionLumaRowShift = 5;
constexpr s32 PredictionChromaRowShift = 4;
constexpr s32 PredictionChroma = LumaRows << PredictionLumaRowShift;

// (vector * distance + (vector > 0)) / 2: a vector scaled to a field one or three away, rounded away from zero
s32 DualPrimeScaled(s32 vector, s32 distance)
{
    return (vector * distance + (vector > 0 ? 1 : 0)) >> 1;
}

// The rows of a block from the macroblock it starts in (rows at most), the rest from the one below: with a half pixel vertical
// vector one more row is read
void SplitRows(MpegPredictionBlock* block, s32 row, s32 rows, s32 macroblockRows, s32 halfDown, s32 fieldPrediction)
{
    if (row + (rows << fieldPrediction) <= macroblockRows - halfDown)
    {
        block->rows = rows;
        block->rowsBelow = 0;
        return;
    }

    s32 above = (macroblockRows >> fieldPrediction) - (row >> fieldPrediction) - halfDown;
    block->rows = above;
    block->rowsBelow = rows - above;
}

void WaitScratchpadDma()
{
    while ((*R_EE_D9_CHCR & ChcrStart) != 0)
    {
    }
}
}

void DualPrimeVectors(MpegSystem* sys, MpegVector* vectors, const s32* dmVector, s32 vectorX, s32 vectorY)
{
    // MPEG-2's 7.6.3.6: the vector scaled to the other parity's field, plus the differential, half a line up or down
    if (sys->pictureStructure == MpegFrame)
    {
        if (sys->topFieldFirst != 0)
        {
            vectors[0].horizontal = DualPrimeScaled(vectorX, 1) + dmVector[0];
            vectors[0].vertical = DualPrimeScaled(vectorY, 1) + dmVector[1] - 1;
            vectors[1].horizontal = DualPrimeScaled(vectorX, 3) + dmVector[0];
            vectors[1].vertical = DualPrimeScaled(vectorY, 3) + dmVector[1] + 1;
        }
        else
        {
            vectors[0].horizontal = DualPrimeScaled(vectorX, 3) + dmVector[0];
            vectors[0].vertical = DualPrimeScaled(vectorY, 3) + dmVector[1] - 1;
            vectors[1].horizontal = DualPrimeScaled(vectorX, 1) + dmVector[0];
            vectors[1].vertical = DualPrimeScaled(vectorY, 1) + dmVector[1] + 1;
        }

        return;
    }

    vectors[0].horizontal = DualPrimeScaled(vectorX, 1) + dmVector[0];
    s32 vertical = DualPrimeScaled(vectorY, 1) + dmVector[1];
    vectors[0].vertical = sys->pictureStructure == MpegTopField ? vertical - 1 : vertical + 1;
}

void GetReferences(MpegSystem* sys, s32 x, s32 y, s32 type, s32 motionType, MpegVector predictors[2][2], s32 fieldSelect[2][2],
                   s32* dmVector)
{
    sys->buffers[sys->bufferIndex].referenceCount = 0;
    MacroblockType macroblock = {static_cast<u32>(type)};
    // A backward prediction is averaged with the forward one
    s32 forward = 0;
    if (macroblock.forward || sys->pictureCodingType == MpegPictureP)
    {
        MpegVector* first = &predictors[0][0];
        MpegVector* second = &predictors[1][0];
        if (sys->pictureStructure == MpegFrame)
        {
            // A P picture's macroblock without vectors has frame prediction
            if (motionType == MpegMotionFrame || !macroblock.forward)
            {
                GetReference(sys, sys->frames[MpegPast], 0, 0, 0, 16, x, y, first->horizontal, first->vertical, 0, 0);
            }
            else if (motionType == MpegMotionField)
            {
                GetReference(sys, sys->frames[MpegPast], fieldSelect[0][0], 0, 0, 8, x, y, first->horizontal, first->vertical >> 1,
                             1, 0);
                GetReference(sys, sys->frames[MpegPast], fieldSelect[1][0], 1, 0, 8, x, y, second->horizontal,
                             second->vertical >> 1, 1, 0);
            }
            else if (motionType == MpegMotionDualPrime)
            {
                // Each field from the field of its parity averaged with the other's
                MpegVector vectors[2];
                DualPrimeVectors(sys, vectors, dmVector, first->horizontal, first->vertical >> 1);
                GetReference(sys, sys->frames[MpegPast], 0, 0, 0, 8, x, y, first->horizontal, first->vertical >> 1, 1, 0);
                GetReference(sys, sys->frames[MpegPast], 1, 0, 0, 8, x, y, vectors[0].horizontal, vectors[0].vertical, 1, 1);
                GetReference(sys, sys->frames[MpegPast], 1, 1, 0, 8, x, y, first->horizontal, first->vertical >> 1, 1, 0);
                GetReference(sys, sys->frames[MpegPast], 0, 1, 0, 8, x, y, vectors[1].horizontal, vectors[1].vertical, 1, 1);
            }
            else
            {
                ErrorValue(sys, g_MpegInvalidMotionType0, motionType);
            }
        }
        else
        {
            // A field picture's references are fields: the past frame's, and in a P picture's second field the frame's first
            // field when it's of the other parity
            s32 bottom = sys->pictureStructure == MpegBottomField;
            MpegImage* fields[2][2] = {{sys->topFields[MpegPast], sys->bottomFields[MpegPast]},
                                       {sys->topFields[MpegFuture], sys->bottomFields[MpegFuture]}};
            s32 sameFrame = 0;
            if (sys->pictureCodingType == MpegPictureP && sys->secondFieldMissing != 0)
            {
                sameFrame = (bottom ^ fieldSelect[0][0]) != 0;
            }

            if (motionType == MpegMotionField || !macroblock.forward)
            {
                GetReference(sys, fields[sameFrame][fieldSelect[0][0]], 0, 0, 0, 16, x, y, first->horizontal, first->vertical,
                             0, 0);
            }
            else if (motionType == MpegMotion16x8)
            {
                GetReference(sys, fields[sameFrame][fieldSelect[0][0]], 0, 0, 0, 8, x, y, first->horizontal, first->vertical, 0,
                             0);
                sameFrame = 0;
                if (sys->pictureCodingType == MpegPictureP && sys->secondFieldMissing != 0)
                {
                    sameFrame = (bottom ^ fieldSelect[1][0]) != 0;
                }

                GetReference(sys, fields[sameFrame][fieldSelect[1][0]], 0, 0, 8, 8, x, y, second->horizontal, second->vertical,
                             0, 0);
            }
            else if (motionType == MpegMotionDualPrime)
            {
                sameFrame = sys->secondFieldMissing != 0;
                MpegVector vectors[2];
                DualPrimeVectors(sys, vectors, dmVector, first->horizontal, first->vertical);
                GetReference(sys, fields[0][bottom], 0, 0, 0, 16, x, y, first->horizontal, first->vertical, 0, 0);
                GetReference(sys, fields[sameFrame][bottom == 0], 0, 0, 0, 16, x, y, vectors[0].horizontal, vectors[0].vertical,
                             0, 1);
            }
            else
            {
                ErrorValue(sys, g_MpegInvalidMotionType1, motionType);
            }
        }

        forward = 1;
    }

    if (!macroblock.backward)
    {
        return;
    }

    MpegVector* first = &predictors[0][1];
    MpegVector* second = &predictors[1][1];
    if (sys->pictureStructure == MpegFrame)
    {
        if (motionType == MpegMotionFrame)
        {
            GetReference(sys, sys->frames[MpegFuture], 0, 0, 0, 16, x, y, first->horizontal, first->vertical, 0, forward);
        }
        else
        {
            // Field prediction, and so is any other motion type (B pictures have no dual prime)
            GetReference(sys, sys->frames[MpegFuture], fieldSelect[0][1], 0, 0, 8, x, y, first->horizontal,
                         first->vertical >> 1, 1, forward);
            GetReference(sys, sys->frames[MpegFuture], fieldSelect[1][1], 1, 0, 8, x, y, second->horizontal,
                         second->vertical >> 1, 1, forward);
        }
    }
    else if (motionType == MpegMotionField)
    {
        GetReference(sys, fieldSelect[0][1] != 0 ? sys->bottomFields[MpegFuture] : sys->topFields[MpegFuture], 0, 0, 0, 16, x, y,
                     first->horizontal, first->vertical, 0, forward);
    }
    else if (motionType == MpegMotion16x8)
    {
        GetReference(sys, fieldSelect[0][1] != 0 ? sys->bottomFields[MpegFuture] : sys->topFields[MpegFuture], 0, 0, 0, 8, x, y,
                     first->horizontal, first->vertical, 0, forward);
        GetReference(sys, fieldSelect[1][1] != 0 ? sys->bottomFields[MpegFuture] : sys->topFields[MpegFuture], 0, 0, 8, 8, x, y,
                     second->horizontal, second->vertical, 0, forward);
    }
    else
    {
        ErrorValue(sys, g_MpegInvalidMotionType2, motionType);
    }
}

s32 MotionCompensate(MpegSystem* sys, s32 address, s32 increment, s32 type, s32 motionType, MpegVector predictors[2][2],
                     s32 fieldSelect[2][2], s32* dmVector)
{
    MacroblockType macroblock = {static_cast<u32>(type)};
    s32 intra = macroblock.intra;
    s32 row = address / sys->widthMacroblocks;
    s32 column = address % sys->widthMacroblocks;
    if (intra != 0)
    {
        WaitScratchpadDma();
        sys->buffers[sys->bufferIndex].predicted = 0;
    }
    else
    {
        if (static_cast<u32>(motionType - MpegMotionField) > MpegMotionDualPrime - MpegMotionField)
        {
            ErrorValue(sys, g_MpegMotionTypeIgnored, motionType);
            sys->macroblockError = 1;
            return 0;
        }

        GetReferences(sys, column << LumaShift, row << LumaShift, type, motionType, predictors, fieldSelect, dmVector);
        // The macroblock before's references are in the scratchpad until it's finished
        WaitScratchpadDma();
        FetchReferences(sys);
        sys->buffers[sys->bufferIndex].predicted = 1;
    }

    sys->buffers[sys->bufferIndex].unused134 = increment == 1 && macroblock.pattern;
    sys->buffers[sys->bufferIndex].intra = intra;
    MpegImage* image;
    if (sys->pictureStructure == MpegFrame)
    {
        image = sys->frames[MpegCurrent];
    }
    else
    {
        image = sys->pictureStructure == MpegBottomField ? sys->bottomFields[MpegCurrent] : sys->topFields[MpegCurrent];
    }

    sys->buffers[sys->bufferIndex].destination = image->pixels + (column * image->heightMacroblocks + row) * MacroblockBytes;
    return 1;
}

void GetReference(MpegSystem* sys, MpegImage* image, s32 field, s32 destinationField, s32 destinationRow, s32 height, s32 x,
                  s32 y, s32 vectorX, s32 vectorY, s32 fieldPrediction, s32 average)
{
    MpegMacroblockBuffers* buffer = &sys->buffers[sys->bufferIndex];
    s32 reference = buffer->referenceCount;
    u8* prediction = sys->prediction;
    MpegPredictionBlock* luma = &buffer->luma[reference];
    MpegPredictionBlock* chroma = &buffer->chroma[reference];

    // The luma's block, in pixels (a field's rows are every other one of the frame's)
    HalfPixels across = {vectorX};
    HalfPixels downwards = {vectorY};
    s32 lumaX = x + across.whole;
    s32 down = downwards.whole;
    if (fieldPrediction != 0)
    {
        down <<= 1;
    }

    s32 lumaY = y + down + destinationRow + field;
    // The picture's pixels are the vector's only places: a vector pointing past an edge repeats the edge's pixel (the sample is
    // clamped in the prediction's rows), so its macroblock is the clamped one's
    s32 pictureRow = lumaY < 0 ? 0 : lumaY >= image->height ? image->height - 1 : lumaY;
    s32 column = lumaX < 0 ? 0 : lumaX >= image->width ? image->width - 1 : lumaX;
    column >>= LumaShift;
    s32 row = pictureRow >> LumaShift;
    s32 index = column * image->heightMacroblocks + row;
    u8* source = image->pixels + index * MacroblockBytes;
    u8* rightSource = image->pixels + ((column < image->widthMacroblocks - 1 ? column + 1 : column) * image->heightMacroblocks
                                        + row) * MacroblockBytes;
    s32 lumaRow = pictureRow - (row << LumaShift);
    s32 halfAcross = across.half;
    s32 halfDown = downwards.half;
    luma->prediction = prediction + ((destinationField + destinationRow) << PredictionLumaRowShift);
    luma->column = lumaX - (column << LumaShift);
    buffer->sources[reference] = source;
    buffer->rightSources[reference] = rightSource;
    luma->xBase = column << LumaShift;
    luma->xMax = image->width - 1;
    luma->yBase = (pictureRow & ~((1 << LumaShift) - 1)) + lumaRow;
    luma->yMax = image->height - 1;
    SplitRows(luma, lumaRow, height, LumaRows, halfDown, fieldPrediction);
    u8* fetched;
    if (sys->bufferType == MpegBuffersScratchpad)
    {
        fetched = buffer->references + reference * FetchedReferenceBytes;
        luma->left = fetched + lumaRow * LumaRowBytes;
        luma->right = fetched + lumaRow * LumaRowBytes + ColumnBytes;
    }
    else
    {
        fetched = source;
        luma->left = source + lumaRow * LumaRowBytes;
        luma->right = rightSource + lumaRow * LumaRowBytes;
    }

    // The rows past the block's own start in the one below's macroblock: the block's own start row tells when (the plane's own
    // rows run out while the block is still made)
    luma->startRow = lumaRow;
    luma->stride = LumaRowBytes << fieldPrediction;

    // The chroma's, of half the size and half the vector (rounded towards zero)
    HalfPixels chromaAcross = {vectorX / 2};
    HalfPixels chromaDownwards = {vectorY / 2};
    s32 chromaX = chromaAcross.whole + (x >> 1);
    s32 chromaDown = chromaDownwards.whole;
    if (fieldPrediction != 0)
    {
        chromaDown <<= 1;
    }

    s32 chromaY = chromaDown + (y >> 1) + (destinationRow >> 1) + field;
    s32 pictureColumns = image->width >> 1;
    s32 pictureRows = image->height >> 1;
    s32 pictureChromaRow = chromaY < 0 ? 0 : chromaY >= pictureRows ? pictureRows - 1 : chromaY;
    s32 chromaColumn = chromaX < 0 ? 0 : chromaX >= pictureColumns ? pictureColumns - 1 : chromaX;
    chromaColumn >>= ChromaShift;
    s32 chromaRow = pictureChromaRow >> ChromaShift;
    s32 chromaRowInBlock = pictureChromaRow - (chromaRow << ChromaShift);
    chroma->column = chromaX - (chromaColumn << ChromaShift);
    chroma->prediction =
        prediction + ((destinationField + (destinationRow >> 1)) << PredictionChromaRowShift) + PredictionChroma;
    s32 chromaHalfAcross = chromaAcross.half;
    s32 chromaHalfDown = chromaDownwards.half;
    chroma->xBase = chromaColumn << ChromaShift;
    chroma->xMax = pictureColumns - 1;
    chroma->yBase = (pictureChromaRow & ~((1 << ChromaShift) - 1)) + chromaRowInBlock;
    chroma->yMax = pictureRows - 1;
    chroma->startRow = chromaRowInBlock;
    SplitRows(chroma, chromaRowInBlock, height >> 1, ChromaRows, chromaHalfDown, fieldPrediction);
    if (sys->bufferType == MpegBuffersScratchpad)
    {
        // The chroma's macroblock among the four fetched (it can be right of the luma's)
        s32 offset = ((chromaColumn - column) * 2 + (chromaRow - row)) * MacroblockBytes;
        chroma->left = fetched + offset + chromaRowInBlock * ChromaRowBytes + LumaBytes;
        chroma->right = fetched + offset + chromaRowInBlock * ChromaRowBytes + ColumnBytes + LumaBytes;
    }
    else
    {
        // The chroma's own macroblock: a vector's whole chroma part can point at a different row or column than the luma's, so
        // it is found on its own, in the plane's macroblock and the one to its right (the picture's last column has none)
        u8* chromaSource = image->pixels + (chromaColumn * image->heightMacroblocks + chromaRow) * MacroblockBytes;
        u8* chromaRightSource = image->pixels
                                + ((chromaColumn < image->widthMacroblocks - 1 ? chromaColumn + 1 : chromaColumn)
                                   * image->heightMacroblocks
                                   + chromaRow) * MacroblockBytes;
        chroma->left = chromaSource + chromaRowInBlock * ChromaRowBytes + LumaBytes;
        chroma->right = chromaRightSource + chromaRowInBlock * ChromaRowBytes + LumaBytes;
    }

    chroma->stride = ChromaRowBytes << fieldPrediction;
    buffer->lumaRoutines[reference] = g_SoftwarePredictionRoutines[average << 2 | halfAcross << 1 | halfDown];
    sys->buffers[sys->bufferIndex].referenceCount++;
    buffer->chromaRoutines[reference] =
        g_SoftwarePredictionRoutines[ChromaRoutines + (average << 2 | chromaHalfAcross << 1 | chromaHalfDown)];
}

void StoreMacroblock(MpegSystem* sys, s32 index)
{
    MpegMacroblockBuffers* buffer = &sys->buffers[index];
    if (buffer->predicted != 0)
    {
        for (s32 reference = 0; reference < buffer->referenceCount; reference++)
        {
            buffer->lumaRoutines[reference](&buffer->luma[reference]);
            buffer->chromaRoutines[reference](&buffer->chroma[reference]);
        }
    }

    if (buffer->intra != 0 && buffer->noResidual != 0)
    {
        Error(sys, g_MpegIntraWithoutBlocks);
    }

    if (buffer->intra != 0)
    {
        StorePixels(buffer->destination, reinterpret_cast<s16*>(buffer->macroblocks));
    }
    else if (buffer->noResidual != 0)
    {
        StorePixels(buffer->destination, reinterpret_cast<s16*>(sys->prediction));
    }
    else
    {
        AddPrediction(buffer->destination, reinterpret_cast<s16*>(sys->prediction), reinterpret_cast<s16*>(buffer->macroblocks));
    }
}


// The references are read from the pictures themselves (the decoder keeps its own buffers): there is nothing to fetch into the
// scratchpad, and no transfer to wait for
void FetchReferences(MpegSystem* sys)
{
    (void)sys;
}

namespace
{
// A byte of a row at the pair's own place: the plane's pixel, drawn back into the picture at its edges (the edge pixels repeat),
// read from the pair's first macroblock or the one to its right
template <int Width>
inline s32 SourceByte(const u8* left, const u8* right, s32 xBase, s32 xMax, s32 pixel)
{
    if (pixel < 0)
    {
        pixel = 0;
    }
    else if (pixel > xMax)
    {
        pixel = xMax;
    }

    s32 index = pixel - xBase;
    return index < Width ? left[index] : right[index - Width];
}

// One output row: width pixels, each the source's, half a pixel across and/or down averaged, and averaged again with the
// prediction already there. The rows are the predicted plane's own, half a pixel more included
template <int Width, bool HalfAcross, bool HalfDown, bool Average>
void PredictRow(s16* output, const u8* topLeft, const u8* topRight, const u8* bottomLeft, const u8* bottomRight,
                const MpegPredictionBlock* block)
{
    s32 xBase = block->xBase;
    s32 xMax = block->xMax;
    s32 column = block->column;
    // A half pixel's average rounds up; a wide and low one together rounds by half their average's 2 (a sum of four's 2)
    constexpr s32 Round = HalfAcross && HalfDown ? 2 : HalfAcross || HalfDown ? 1 : 0;
    constexpr s32 Shift = HalfAcross && HalfDown ? 2 : HalfAcross || HalfDown ? 1 : 0;
    for (s32 x = 0; x < Width; x++)
    {
        s32 pixel = xBase + column + x;
        s32 value = SourceByte<Width>(topLeft, topRight, xBase, xMax, pixel);
        if (HalfAcross)
        {
            value += SourceByte<Width>(topLeft, topRight, xBase, xMax, pixel + 1);
        }

        if (HalfDown)
        {
            value += SourceByte<Width>(bottomLeft, bottomRight, xBase, xMax, pixel);
            if (HalfAcross)
            {
                value += SourceByte<Width>(bottomLeft, bottomRight, xBase, xMax, pixel + 1);
            }
        }

        value = (value + Round) >> Shift;
        if (Average)
        {
            value = (value + output[x] + 1) >> 1;
        }

        output[x] = static_cast<s16>(value);
    }
}

// A plane's rows: each the block's row, continuing past the plane's own macroblock into the one below (the gap its other
// planes leave). The rows past the picture's own come back to its edges: the pixel's row drawn back to its neighbours'
template <int Width, int Gap, bool HalfAcross, bool HalfDown, bool Average>
void PredictPlane(s16* prediction, const u8* left, const u8* right, const MpegPredictionBlock* block)
{
    // A row of the block is one of the plane's rows, or two (a field's are every other one of the plane's)
    s32 rowStep = block->stride / Width;
    s32 startRow = block->startRow;
    s32 yBase = block->yBase;
    s32 yMax = block->yMax;
    s32 count = block->rows + block->rowsBelow;
    for (s32 row = 0; row < count; row++)
    {
        s32 rowTop = yBase + row * rowStep;
        s32 rowBottom = rowTop + rowStep;
        // The rows' own places, drawn back into the plane's own rows, and through them the block's bytes' places (past the
        // plane's own macroblock's rows the ones below's follow, each of their other planes' bytes between)
        s32 topIndex = ((rowTop < 0 ? 0 : rowTop > yMax ? yMax : rowTop) - yBase) / rowStep;
        s32 bottomIndex = ((rowBottom < 0 ? 0 : rowBottom > yMax ? yMax : rowBottom) - yBase) / rowStep;
        const u8* top = left + topIndex * block->stride + ((startRow + topIndex * rowStep) / Width) * Gap;
        const u8* topRight = right + topIndex * block->stride + ((startRow + topIndex * rowStep) / Width) * Gap;
        const u8* bottom = left + bottomIndex * block->stride + ((startRow + bottomIndex * rowStep) / Width) * Gap;
        const u8* bottomRight = right + bottomIndex * block->stride + ((startRow + bottomIndex * rowStep) / Width) * Gap;
        // The block's rows go into the prediction's own rows: a frame's row each, a field's every other one of the macroblock's
        // (the other parity's reference fills the rows between)
        PredictRow<Width, HalfAcross, HalfDown, Average>(prediction + row * rowStep * Width, top, topRight, bottom,
                                                         bottomRight, block);
    }
}

// The chroma's two planes (Cb, then Cr): each is Width wide and a plane of its own
template <int Width, int Gap, bool HalfAcross, bool HalfDown, bool Average>
void PredictChromaPlanes(const MpegPredictionBlock* block)
{
    for (s32 plane = 0; plane < 2; plane++)
    {
        PredictPlane<Width, Gap, HalfAcross, HalfDown, Average>(
            reinterpret_cast<s16*>(block->prediction) + plane * Width * Width, block->left + plane * Width * Width,
            block->right + plane * Width * Width, block);
    }
}

constexpr s32 LumaWidth = 16;
constexpr s32 ChromaWidth = 8;
// The bytes between a macroblock's luma and the one below's luma (its chroma), and between a chroma plane and the one below's
constexpr s32 LumaGap = 128;
constexpr s32 ChromaGap = 320;

void PredictLuma(const MpegPredictionBlock* block)
{
    PredictPlane<LumaWidth, LumaGap, false, false, false>(reinterpret_cast<s16*>(block->prediction), block->left,
                                                          block->right, block);
}

void PredictLumaHalfDown(const MpegPredictionBlock* block)
{
    PredictPlane<LumaWidth, LumaGap, false, true, false>(reinterpret_cast<s16*>(block->prediction), block->left,
                                                         block->right, block);
}

void PredictLumaHalfAcross(const MpegPredictionBlock* block)
{
    PredictPlane<LumaWidth, LumaGap, true, false, false>(reinterpret_cast<s16*>(block->prediction), block->left,
                                                         block->right, block);
}

void PredictLumaHalfBoth(const MpegPredictionBlock* block)
{
    PredictPlane<LumaWidth, LumaGap, true, true, false>(reinterpret_cast<s16*>(block->prediction), block->left,
                                                        block->right, block);
}

void AverageLuma(const MpegPredictionBlock* block)
{
    PredictPlane<LumaWidth, LumaGap, false, false, true>(reinterpret_cast<s16*>(block->prediction), block->left,
                                                         block->right, block);
}

void AverageLumaHalfDown(const MpegPredictionBlock* block)
{
    PredictPlane<LumaWidth, LumaGap, false, true, true>(reinterpret_cast<s16*>(block->prediction), block->left,
                                                        block->right, block);
}

void AverageLumaHalfAcross(const MpegPredictionBlock* block)
{
    PredictPlane<LumaWidth, LumaGap, true, false, true>(reinterpret_cast<s16*>(block->prediction), block->left,
                                                        block->right, block);
}

void AverageLumaHalfBoth(const MpegPredictionBlock* block)
{
    PredictPlane<LumaWidth, LumaGap, true, true, true>(reinterpret_cast<s16*>(block->prediction), block->left,
                                                       block->right, block);
}

void PredictChroma(const MpegPredictionBlock* block)
{
    PredictChromaPlanes<ChromaWidth, ChromaGap, false, false, false>(block);
}

void PredictChromaHalfDown(const MpegPredictionBlock* block)
{
    PredictChromaPlanes<ChromaWidth, ChromaGap, false, true, false>(block);
}

void PredictChromaHalfAcross(const MpegPredictionBlock* block)
{
    PredictChromaPlanes<ChromaWidth, ChromaGap, true, false, false>(block);
}

void PredictChromaHalfBoth(const MpegPredictionBlock* block)
{
    PredictChromaPlanes<ChromaWidth, ChromaGap, true, true, false>(block);
}

void AverageChroma(const MpegPredictionBlock* block)
{
    PredictChromaPlanes<ChromaWidth, ChromaGap, false, false, true>(block);
}

void AverageChromaHalfDown(const MpegPredictionBlock* block)
{
    PredictChromaPlanes<ChromaWidth, ChromaGap, false, true, true>(block);
}

void AverageChromaHalfAcross(const MpegPredictionBlock* block)
{
    PredictChromaPlanes<ChromaWidth, ChromaGap, true, false, true>(block);
}

void AverageChromaHalfBoth(const MpegPredictionBlock* block)
{
    PredictChromaPlanes<ChromaWidth, ChromaGap, true, true, true>(block);
}
}

// The prediction routines by average << 2 | half a pixel across << 1 | half a pixel down (retail's table, in software)
MpegPredictionRoutine g_SoftwarePredictionRoutines[16] =
{
    PredictLuma, PredictLumaHalfDown, PredictLumaHalfAcross, PredictLumaHalfBoth,
    AverageLuma, AverageLumaHalfDown, AverageLumaHalfAcross, AverageLumaHalfBoth,
    PredictChroma, PredictChromaHalfDown, PredictChromaHalfAcross, PredictChromaHalfBoth,
    AverageChroma, AverageChromaHalfDown, AverageChromaHalfAcross, AverageChromaHalfBoth,
};

// A macroblock's six blocks of 64 pixels: the prediction and the residual added, crammed into bytes
void AddPrediction(u8* destination, const s16* prediction, const s16* residual)
{
    for (s32 i = 0; i < static_cast<s32>(MacroblockBytes); i++)
    {
        s32 value = prediction[i] + residual[i];
        destination[i] = static_cast<u8>(value < 0 ? 0 : (value > 255 ? 255 : value));
    }
}

// The same without the residual
void StorePixels(u8* destination, const s16* values)
{
    for (s32 i = 0; i < static_cast<s32>(MacroblockBytes); i++)
    {
        s32 value = values[i];
        destination[i] = static_cast<u8>(value < 0 ? 0 : (value > 255 ? 255 : value));
    }
}

}
