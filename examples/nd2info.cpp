#include <iomanip>
#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <cstring>

#include "Nd2ReadSdk.h"

static void dumpattributes(LIMFILEHANDLE h)
{
    LIMSTR out = Lim_FileGetAttributes(h);
    std::cout << (out ? out : "N/A") << std::endl;
    Lim_FileFreeString(out);
}

static void dumpcoordinates(LIMFILEHANDLE h)
{
    LIMSIZE size = Lim_FileGetCoordSize(h);
    if (0 == size)
    {
        std::cout << "N/A" << std::endl;
    }
    else
    {
        LIMUINT count = Lim_FileGetSeqCount(h);
        for (LIMUINT i = 0; i < count; i++)
        {
            std::vector<LIMUINT> v(size);
            Lim_FileGetCoordsFromSeqIndex(h, i, v.data(), v.size());
            std::cout << std::setw(6) << i << ": " << std::setw(4) << v.at(0);
            for (LIMSIZE j = 1; j < size; j++)
            {
                std::cout << ", " << std::setw(4) << v.at(j);
            }
            std::cout << std::endl;
        }
    }
}

static void dumpexperiment(LIMFILEHANDLE h)
{
    LIMSTR out = Lim_FileGetExperiment(h);
    std::cout << (out ? out : "N/A") << std::endl;
    Lim_FileFreeString(out);
}

static void dumpdimensions(LIMFILEHANDLE h)
{
    LIMSIZE size = Lim_FileGetCoordSize(h);
    if (0 == size)
    {
        std::cout << "N/A" << std::endl;
    }
    else
    {
        LIMCHAR buffer[1024];
        for (LIMSIZE i = 0; i < size; i++)
        {
            LIMUINT count = Lim_FileGetCoordInfo(h, static_cast<LIMUINT>(i), buffer, 1024);
            std::cout << std::setw(2) << i << ": " << std::setw(20) << buffer
                      << std::setw(4) << count << std::endl;
        }
    }
}

static void dumpimageinfo(LIMFILEHANDLE h)
{
    LIMPICTURE pic = {0};
    Lim_FileGetImageData(h, 0, &pic);

    std::cout << "width:      " << pic.uiWidth << std::endl;
    std::cout << "height:     " << pic.uiHeight << std::endl;
    std::cout << "components: " << pic.uiComponents << std::endl;
    std::cout << "bitdepth:   " << pic.uiBitsPerComp << std::endl;

    Lim_DestroyPicture(&pic);
}

static void dumpmetadata(LIMFILEHANDLE h)
{
    LIMSTR out = Lim_FileGetMetadata(h);
    std::cout << (out ? out : "N/A") << std::endl;
    Lim_FileFreeString(out);
}

static void dumpstack(LIMFILEHANDLE h, const std::string& filename)
{
    std::fstream fs(filename, std::ios::binary | std::ios::out | std::ios::trunc);

    LIMPICTURE pic = {0};
    LIMUINT count = Lim_FileGetSeqCount(h);
    for (LIMUINT i = 0; i < count; i++)
    {
        if (LIM_OK != Lim_FileGetImageData(h, i, &pic))
        {
            break;
        }

        const size_t bytes = static_cast<size_t>(pic.uiWidth) * pic.uiComponents *
                            ((pic.uiBitsPerComp + 7) / 8);
        for (LIMUINT j = 0; j < pic.uiHeight; j++)
        {
            fs.write(static_cast<const char*>(pic.pImageData) + j * pic.uiWidthBytes, bytes);
        }
    }

    fs.close();
    Lim_DestroyPicture(&pic);
}

static void dumptextinfo(LIMFILEHANDLE h)
{
    LIMSTR out = Lim_FileGetTextinfo(h);
    std::cout << (out ? out : "N/A") << std::endl;
    Lim_FileFreeString(out);
}

int main(int argc, char** argv)
{
    if (3 == argc && 0 == std::strcmp(argv[1], "allinfo"))
    {
        LIMFILEHANDLE h = Lim_FileOpenForReadUtf8(argv[2]);
        if (!h)
        {
            std::cerr << "Cannot open file!" << std::endl;
        }
        else
        {
            std::cout << "Attributes:" << std::endl;
            dumpattributes(h);
            std::cout << "Textinfo:" << std::endl;
            dumptextinfo(h);
            std::cout << "Metadata:" << std::endl;
            dumpmetadata(h);
            std::cout << "Experiment:" << std::endl;
            dumpexperiment(h);
            std::cout << "Dimensions:" << std::endl;
            dumpdimensions(h);
            dumpimageinfo(h);
            Lim_FileClose(h);
        }
    }
    else if (3 == argc && 0 == std::strcmp(argv[1], "attributes"))
    {
        LIMFILEHANDLE h = Lim_FileOpenForReadUtf8(argv[2]);
        if (!h)
        {
            std::cerr << "Cannot open file!" << std::endl;
        }
        else
        {
            dumpattributes(h);
            Lim_FileClose(h);
        }
    }
    else if (3 == argc && 0 == std::strcmp(argv[1], "coordinates"))
    {
        LIMFILEHANDLE h = Lim_FileOpenForReadUtf8(argv[2]);
        if (!h)
        {
            std::cerr << "Cannot open file!" << std::endl;
        }
        else
        {
            dumpcoordinates(h);
            Lim_FileClose(h);
        }
    }
    else if (3 == argc && 0 == std::strcmp(argv[1], "dimensions"))
    {
        LIMFILEHANDLE h = Lim_FileOpenForReadUtf8(argv[2]);
        if (!h)
        {
            std::cerr << "Cannot open file!" << std::endl;
        }
        else
        {
            dumpdimensions(h);
            Lim_FileClose(h);
        }
    }
    else if (4 == argc && 0 == std::strcmp(argv[1], "dumpallimages"))
    {
        LIMFILEHANDLE h = Lim_FileOpenForReadUtf8(argv[3]);
        if (!h)
        {
            std::cerr << "Cannot open file!" << std::endl;
        }
        else
        {
            dumpstack(h, argv[2]);
            Lim_FileClose(h);
        }
    }
    else if (3 == argc && 0 == std::strcmp(argv[1], "experiment"))
    {
        LIMFILEHANDLE h = Lim_FileOpenForReadUtf8(argv[2]);
        if (!h)
        {
            std::cerr << "Cannot open file!" << std::endl;
        }
        else
        {
            dumpexperiment(h);
            Lim_FileClose(h);
        }
    }
    else if (3 == argc && 0 == std::strcmp(argv[1], "imageinfo"))
    {
        LIMFILEHANDLE h = Lim_FileOpenForReadUtf8(argv[2]);
        if (!h)
        {
            std::cerr << "Cannot open file!" << std::endl;
        }
        else
        {
            dumpimageinfo(h);
            Lim_FileClose(h);
        }
    }
    else if (3 == argc && 0 == std::strcmp(argv[1], "metadata"))
    {
        LIMFILEHANDLE h = Lim_FileOpenForReadUtf8(argv[2]);
        if (!h)
        {
            std::cerr << "Cannot open file!" << std::endl;
        }
        else
        {
            dumpmetadata(h);
            Lim_FileClose(h);
        }
    }
    else if (4 == argc && 0 == std::strcmp(argv[1], "metadata"))
    {
        LIMFILEHANDLE h = Lim_FileOpenForReadUtf8(argv[3]);
        if (!h)
        {
            std::cerr << "Cannot open file!" << std::endl;
        }
        else
        {
            auto seqIndex = 0;
            try
            {
                seqIndex = std::stoi(argv[2]);
            }
            catch (...)
            {
                std::cerr << "Cannot parse seqIndex from '" << argv[2] << "'!" << std::endl;
            }

            LIMSTR out = Lim_FileGetFrameMetadata(h, static_cast<LIMUINT>(seqIndex));
            std::cout << (out ? out : "N/A") << std::endl;
            Lim_FileFreeString(out);
            Lim_FileClose(h);
        }
    }
    else if (3 == argc && 0 == std::strcmp(argv[1], "textinfo"))
    {
        LIMFILEHANDLE h = Lim_FileOpenForReadUtf8(argv[2]);
        if (!h)
        {
            std::cerr << "Cannot open file!" << std::endl;
        }
        else
        {
            dumptextinfo(h);
            Lim_FileClose(h);
        }
    }
    else
    {
        std::cerr << "Usage: " << argv[0] << " cmd [options] file.nd2" << std::endl;
        std::cerr << "cmd:" << std::endl;
        std::cerr << "    allinfo <no options> - prints all metadata" << std::endl;
        std::cerr << "    attributes <no options> - prints attributes" << std::endl;
        std::cerr << "    coordinates <no options> - prints all frame coordinates" << std::endl;
        std::cerr << "    dimensions <no options> - prints coordinate dimensions" << std::endl;
        std::cerr << "    dumpallimages filename - makes superstack of all images" << std::endl;
        std::cerr << "    experiment <no options> - prints experiment" << std::endl;
        std::cerr << "    imageinfo <no options> - prints image size" << std::endl;
        std::cerr << "    metadata <no options> - prints global metadata" << std::endl;
        std::cerr << "    metadata seqIndex - prints global metadata merged with frame metadata" << std::endl;
        std::cerr << "    textinfo <no options> - prints textinfo" << std::endl;
    }

    return 0;
}
