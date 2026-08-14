#include "importscpt.hpp"

#include <components/esm3/esmreader.hpp>

namespace ESSImport
{

    void SCPT::load(ESM::ESMReader& esm)
    {
        esm.getHNT("SCHD", mSCHD.mName.mData, mSCHD.mNumShorts, mSCHD.mNumLongs, mSCHD.mNumFloats,
            mSCHD.mScriptDataSize, mSCHD.mStringTableSize);

        mSCRI.load(esm);

<<<<<<< HEAD
        if (esm.isNextSub("RNAM"))
        {
            mRunning = true;
            ESM::FormId32 refNum;
            esm.getHT(refNum);
            mRefNum = ESM::RefNum::fromUint32(refNum);
            mRefNum.mContentFile--;
=======
        mRefNum = -1;
        if (esm.isNextSub("RNAM"))
        {
            mRunning = true;
            esm.getHT(mRefNum);
>>>>>>> origin/main
        }
        else
            mRunning = false;
    }

}
