SET(TARGET_H
    GLTFReader.h
    GLTFWriter.h
#    B3DMReader.h
#    B3DMWriter.h
)

SET(TARGET_SRC
    ReaderWriterGLTF.cpp
)

INCLUDE_DIRECTORIES(tinygltf rapidjson/include/rapidjson)

#### end var setup  ###
SETUP_PLUGIN(gltf)
