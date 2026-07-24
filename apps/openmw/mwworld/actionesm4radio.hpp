#ifndef GAME_MWWORLD_ACTIONESM4RADIO_H
#define GAME_MWWORLD_ACTIONESM4RADIO_H

#include <string>

#include "action.hpp"

namespace MWWorld
{
    class ActionEsm4Radio final : public Action
    {
        ESM::RefId mBroadcastSound;
        ESM::RefId mStation;
        std::string mBroadcastVoice;

        void executeImp(const Ptr& actor) override;

    public:
        ActionEsm4Radio(
            const Ptr& target, ESM::RefId activationSound, ESM::RefId broadcastSound, ESM::RefId station,
            std::string broadcastVoice = {});
    };
}

#endif
