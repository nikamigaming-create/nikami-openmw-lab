#ifndef OPENMW_MWSOUND_MOVIEAUDIOFACTORY_H
#define OPENMW_MWSOUND_MOVIEAUDIOFACTORY_H

<<<<<<< HEAD
#include <osg-ffmpeg-videoplayer/audiofactory.hpp>
=======
#include <extern/osg-ffmpeg-videoplayer/audiofactory.hpp>
>>>>>>> origin/main

namespace MWSound
{

    class MovieAudioFactory : public Video::MovieAudioFactory
    {
        std::unique_ptr<Video::MovieAudioDecoder> createDecoder(Video::VideoState* videoState) override;
    };

}

#endif
