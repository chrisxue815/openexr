///////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2003, Industrial Light & Magic, a division of Lucas
// Digital Ltd. LLC
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
// *       Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
// *       Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
// *       Neither the name of Industrial Light & Magic nor the names of
// its contributors may be used to endorse or promote products derived
// from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
///////////////////////////////////////////////////////////////////////////



//-----------------------------------------------------------------------------
//
//	class InputFile
//
//-----------------------------------------------------------------------------

#include <ImfInputFile.h>
#include <ImfScanLineInputFile.h>
#include <ImfTiledInputFile.h>
#include <ImfChannelList.h>
#include <ImfMisc.h>
#include <Iex.h>
#include <fstream>
#include <algorithm>
#include <ImfVersion.h>
#include <half.h>

#if defined PLATFORM_WIN32
namespace
{
template<class T>
inline T min (const T &a, const T &b) { return (a <= b) ? a : b; }

template<class T>
inline T max (const T &a, const T &b) { return (a >= b) ? a : b; }
}
#endif

namespace Imf {

using std::ifstream;
using Imath::Box2i;


// stores things that will be needed between calls to readPixels
struct InputFile::Data
{
public:    
    std::string         fileName;
    Header              header;
    int                 version;
    std::ifstream       is;

    TiledInputFile*     tFile;
    ScanLineInputFile*  sFile;

    LineOrder           lineOrder;      // the file's lineorder
    int                 minY;           // data window's min y coord
    int                 maxY;           // data window's max x coord
    
    FrameBuffer*        cachedBuffer;
    
    int                 cachedTileY;
    int                 offset;

    Data () : tFile(0), sFile(0), cachedBuffer(0), cachedTileY(-1)
    {
        // empty
    }
    
    ~Data ()
    {
        delete tFile;
        delete sFile;
        
        //
        // If cachedBuffer is not 0 then that means we already have
        // some data buffered. We must delete the memory allocated for
        // the cachedBuffer.
        //

        if (cachedBuffer)
        {
            // delete all the slices in the cached frameBuffer
            for (FrameBuffer::ConstIterator k = cachedBuffer->begin();
                 k != cachedBuffer->end(); ++k)
            {
                Slice s = k.slice();
                switch (s.type)
                {
                    case UINT:
                        delete [] (((unsigned int*)s.base) + offset);
                    break;

                    case HALF:
                    {
                        delete [] ((half*)s.base + offset);
                    }
                    break;

                    case FLOAT:
                        delete [] (((float*)s.base) + offset);
                    break;
                }                
            }
        }
        delete cachedBuffer;        
    }
};


namespace {

void
bufferedReadPixels(InputFile::Data* ifd, int scanLine1, int scanLine2)
{
    //
    // bufferedReadPixels reads each row of tiles that intersect the scan-line
    // range. The previous row of tiles is cached in order to prevent redundent
    // tile reads when accessing scanlines sequentially.
    //

#ifdef PLATFORM_WIN32
    int minY = min (scanLine1, scanLine2);
    int maxY = max (scanLine1, scanLine2);
#else
    int minY = std::min (scanLine1, scanLine2);
    int maxY = std::max (scanLine1, scanLine2);
#endif
    if (minY < ifd->minY || maxY >  ifd->maxY)
    {
        throw Iex::ArgExc ("Tried to read scan line outside "
                    "the image file's data window.");
    }

    //
    // The minimum and maximum y tile coordinates that intersect this
    // scanline range
    //

    int minDy = (minY - ifd->minY)/ifd->tFile->tileYSize();
    int maxDy = (maxY - ifd->minY)/ifd->tFile->tileYSize();

    //
    // Figure out which one is first in the file so we can read without seeking
    //

    int yStart, yEnd, yStep;
    if (ifd->lineOrder == DECREASING_Y)
    {
        yStart = maxDy;
        yEnd = minDy-1;
        yStep = -1;
    }
    else
    {
        yStart = minDy;
        yEnd = maxDy+1;
        yStep = 1;
    }

    // backup the user's framebuffer
    FrameBuffer oldBuffer = ifd->tFile->frameBuffer();

    // the number of pixels in a row of tiles
    int tileRowSize = ifd->tFile->levelWidth(0)*ifd->tFile->tileYSize();

    //
    // Read the tiles into our temporary framebuffer and copy them into
    // the user's buffer
    //

    for (int j = yStart; j != yEnd; j += yStep)
    {
        Box2i tileRange = ifd->tFile->dataWindowForTile(0, j, 0);

#ifdef PLATFORM_WIN32
        int minYThisRow = max (minY, tileRange.min.y);
        int maxYThisRow = min (maxY, tileRange.max.y);
#else
        int minYThisRow = std::max (minY, tileRange.min.y);
        int maxYThisRow = std::min (maxY, tileRange.max.y);
#endif

        if (j != ifd->cachedTileY)
        {
            //
            // We don't have any valid buffered info, so we need to read in
            // from the file.
            //

            //
            // If cachedBuffer is not 0 then that means we already have
            // some data buffered. We must delete the memory allocated for
            // the previous cachedBuffer before continuing.
            //

            if (ifd->cachedBuffer)
            {
                // delete all the slices in the cached frameBuffer
                for (FrameBuffer::ConstIterator k = ifd->cachedBuffer->begin();
                     k != ifd->cachedBuffer->end(); ++k)
                {
                    Slice s = k.slice();
                    switch (s.type)
                    {
                        case UINT:

                            delete [] ((unsigned int*)s.base + ifd->offset);

                        break;

                        case HALF:

                            delete [] ((half*)s.base + ifd->offset);

                        break;

                        case FLOAT:

                            delete [] ((float*)s.base + ifd->offset);

                        break;
                    }                
                }
                delete ifd->cachedBuffer;
            }

            //
            // Then, allocate a framebuffer big enough to store all tiles in
            // this row of tiles and save it as cachedBuffer.
            //

            ifd->cachedBuffer = new FrameBuffer();
            ifd->cachedTileY = j;
            ifd->offset = tileRange.min.y*ifd->tFile->levelWidth(0) +
                          tileRange.min.x;

            for (FrameBuffer::ConstIterator k = oldBuffer.begin();
                 k != oldBuffer.end(); ++k)
            {
                Slice s = k.slice();
                switch (s.type)
                {
                    case UINT:

                        ifd->cachedBuffer->insert(k.name(),
                        Slice(UINT,
                              (char *)(new unsigned int[tileRowSize] -
                                       ifd->offset),
                              sizeof(unsigned int),
                              sizeof(unsigned int)*ifd->tFile->levelWidth(0)));

                    break;

                    case HALF:

                        ifd->cachedBuffer->insert(k.name(),
                        Slice(HALF,
                              (char *)(new half[tileRowSize] -
                                       ifd->offset),
                              sizeof(half),
                              sizeof(half)*ifd->tFile->levelWidth(0)));

                    break;

                    case FLOAT:

                        ifd->cachedBuffer->insert(k.name(),
                        Slice(FLOAT,
                              (char *)(new float[tileRowSize] -
                                       ifd->offset),
                              sizeof(float),
                              sizeof(float)*ifd->tFile->levelWidth(0)));

                    break;

                    default:

                        throw Iex::ArgExc ("Unknown pixel data type.");
                }
            }

            ifd->tFile->setFrameBuffer(*(ifd->cachedBuffer));


            //
            // Read in the whole row of tiles into cachedBuffer.
            //

            for (int i = 0; i < ifd->tFile->numXTiles(0); ++i)
                ifd->tFile->readTile(i, j, 0);
        }

        //
        // Copy the data from our cached framebuffer into the user's
        // framebuffer.
        //

        Box2i levelRange = ifd->tFile->dataWindowForLevel(0);
        for (FrameBuffer::ConstIterator k = ifd->cachedBuffer->begin();
             k != ifd->cachedBuffer->end();
             ++k)
        {
            Slice fromSlice = k.slice();        // slice to write from
            Slice toSlice = oldBuffer[k.name()];// slice to write to

            char *fromPtr, *toPtr;
            int size = pixelTypeSize(toSlice.type);

            for (int y = minYThisRow; y <= maxYThisRow; ++y)
            {
                // set the pointers to the start of the y scanline in
                // this or of tiles
                fromPtr = fromSlice.base +
                          y*fromSlice.yStride +
                          levelRange.min.x*fromSlice.xStride;

                toPtr   = toSlice.base +
                          y*toSlice.yStride +
                          levelRange.min.x*toSlice.xStride;

                // copy all pixels for the scanline in this row of tiles
                for (int x = levelRange.min.x; x <= levelRange.max.x; ++x)
                {
                    for (size_t i = 0; i < size; ++i)
                        toPtr[i] = fromPtr[i];

                    fromPtr += fromSlice.xStride;
                    toPtr += toSlice.xStride;
                }
            }
        }
    }

    // restore the user's original frameBuffer, but with the specified
    // scanlines read in.
    ifd->tFile->setFrameBuffer(oldBuffer);
}

} // namespace



InputFile::InputFile (const char fileName[]) :
    _data(new Data())
{
    try
    {
        _data->fileName = fileName;
#ifndef HAVE_IOS_BASE
        _data->is.open (fileName, std::ios::binary|std::ios::in);
#else
        _data->is.open (fileName, std::ios_base::binary);
#endif
        if (!_data->is)
            Iex::throwErrnoExc();

        _data->header.readFrom (_data->is, _data->version);
        _data->header.sanityCheck(isTiled(_data->version));

        if (isTiled(_data->version))
        {
            _data->lineOrder = _data->header.lineOrder();

            //
            // Save the dataWindow information
            //

            const Box2i &dataWindow = _data->header.dataWindow();
            _data->minY = dataWindow.min.y;
            _data->maxY = dataWindow.max.y;
        
            _data->tFile = new TiledInputFile (fileName,
                                               _data->header,
                                               _data->is);
        }
        else
        {
            _data->sFile = new ScanLineInputFile (fileName,
                                                  _data->header,
                                                  _data->is);
        }
    }
    catch (Iex::BaseExc &e)
    {
        REPLACE_EXC (e, "Cannot read image file \"" << fileName << "\". " << e);
        throw;
    }
}


InputFile::~InputFile ()
{
    delete _data;
}


const char *
InputFile::fileName () const
{
    return _data->fileName.c_str();
}


const Header &
InputFile::header () const
{
    return _data->header;
}


int
InputFile::version () const
{
    return _data->version;
}


void
InputFile::setFrameBuffer (const FrameBuffer &frameBuffer)
{
    if (isTiled (_data->version))
    {
        // Invalidate the cached buffer if the new frameBuffer has
        // different types than the old.
        const ChannelList &channels = _data->header.channels();

        for (FrameBuffer::ConstIterator j = frameBuffer.begin();
             j != frameBuffer.end();
             ++j)
        {
            ChannelList::ConstIterator i = channels.find (j.name());

            //
            // If the new frameBuffer has channels that the old one didn't
            // have, or the channels in the new frameBuffer have different
            // types than the ones in the old frameBuffer, then we must
            // invalidate the cache.
            //

            if (i == channels.end() || i.channel().type != j.slice().type)
            {
                _data->cachedTileY = -1;
                break;
            }
        }
    
        return _data->tFile->setFrameBuffer(frameBuffer);
    }
    else
    {
        return _data->sFile->setFrameBuffer(frameBuffer);
    }
}


const FrameBuffer &
InputFile::frameBuffer () const
{
    if (isTiled (_data->version))
    {
	return _data->tFile->frameBuffer();
    }
    else
    {
	return _data->sFile->frameBuffer();
    }
}


void
InputFile::readPixels (int scanLine1, int scanLine2)
{
    if (isTiled (_data->version))
    {
        bufferedReadPixels(_data, scanLine1, scanLine2);
    }
    else
    {
        _data->sFile->readPixels(scanLine1, scanLine2);
    }
}


void
InputFile::readPixels (int scanLine)
{
    readPixels (scanLine, scanLine);
}


void
InputFile::rawPixelData (int firstScanLine,
			 const char *&pixelData,
			 int &pixelDataSize)
{
    try
    {
	if (isTiled (_data->version))
	{
	    throw Iex::ArgExc ("Tried to read a raw scanline from a tiled "
				" image.");
	}
        
        _data->sFile->rawPixelData(firstScanLine, pixelData, pixelDataSize);
    }
    catch (Iex::BaseExc &e)
    {
	REPLACE_EXC (e, "Error reading pixel data from image "
		        "file \"" << fileName() << "\". " << e);
	throw;
    }
}

void
InputFile::rawTileData (int &dx, int &dy, int &lx, int &ly,
				      const char *&pixelData,
				      int &pixelDataSize)
{
    try
    {
	if (!isTiled (_data->version))
	{
	    throw Iex::ArgExc ("Tried to read a raw tile from a scanline "
				" based image.");
	}
        
        _data->tFile->rawTileData(dx, dy, lx, ly, pixelData, pixelDataSize);
    }
    catch (Iex::BaseExc &e)
    {
	REPLACE_EXC (e, "Error reading tile data from image "
		        "file \"" << fileName() << "\". " << e);
	throw;
    }
}

TiledInputFile*
InputFile::tFile()
{
    try
    {
        if (!isTiled (_data->version))
        {
            throw Iex::ArgExc ("Tried to access a tiled file in an InputFile "
                               "which is not tiled. ");
        }

        return _data->tFile;
    }
    catch (Iex::BaseExc &e)
    {
        REPLACE_EXC (e, "Error reading tile data from image "
                        "file \"" << fileName() << "\". " << e);
        throw;
    }
}


} // namespace Imf
