#include "esm4npcanimation.hpp"

#include <components/esm4/loadarma.hpp>
#include <components/esm4/loadarmo.hpp>
#include <components/esm4/loadclot.hpp>
#include <components/esm4/loadflst.hpp>
#include <components/esm4/loadhair.hpp>
#include <components/esm4/loadhdpt.hpp>
#include <components/esm4/loadnpc.hpp>
#include <components/esm4/loadrace.hpp>

#include <components/misc/resourcehelpers.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>

#include "../mwbase/environment.hpp"
#include "../mwclass/esm4npc.hpp"
#include "../mwworld/esmstore.hpp"

namespace MWRender
{
    ESM4NpcAnimation::ESM4NpcAnimation(
        const MWWorld::Ptr& ptr, osg::ref_ptr<osg::Group> parentNode, Resource::ResourceSystem* resourceSystem)
        : Animation(ptr, std::move(parentNode), resourceSystem)
    {
        setObjectRoot(mPtr.getClass().getCorrectedModel(mPtr), true, true, false);
        updateParts();
    }

    void ESM4NpcAnimation::updateParts()
    {
        if (mObjectRoot == nullptr)
            return;
        const ESM4::Npc* traits = MWClass::ESM4Npc::getTraitsRecord(mPtr);
        if (traits == nullptr)
            return;
        if (traits->mIsTES4)
            updatePartsTES4(*traits);
        else if (traits->mIsFONV)
            updatePartsFONV(*traits);
        else
        {
            // There is no easy way to distinguish TES5 and FO3.
            // In case of FO3 the function shouldn't crash the game and will
            // only lead to the NPC not being rendered.
            updatePartsTES5(*traits);
        }
    }

    void ESM4NpcAnimation::insertPart(std::string_view model)
    {
        if (model.empty())
            return;
        mResourceSystem->getSceneManager()->getInstance(
            Misc::ResourceHelpers::correctMeshPath(VFS::Path::Normalized(model)), mObjectRoot.get());
    }

    template <class Record>
    static std::string_view chooseTes4EquipmentModel(const Record* rec, bool isFemale)
    {
        if (isFemale && !rec->mModelFemale.empty())
            return rec->mModelFemale;
        else if (!isFemale && !rec->mModelMale.empty())
            return rec->mModelMale;
        else
            return rec->mModel;
    }

    void ESM4NpcAnimation::updatePartsTES4(const ESM4::Npc& traits)
    {
        const ESM4::Race* race = MWClass::ESM4Npc::getRace(mPtr);
        bool isFemale = MWClass::ESM4Npc::isFemale(mPtr);

        // TODO: Body and head parts are placed incorrectly, need to attach to bones

        for (const ESM4::Race::BodyPart& bodyPart : (isFemale ? race->mBodyPartsFemale : race->mBodyPartsMale))
            insertPart(bodyPart.mesh);
        for (const ESM4::Race::BodyPart& bodyPart : race->mHeadParts)
            insertPart(bodyPart.mesh);
        if (!traits.mHair.isZeroOrUnset())
        {
            const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
            if (const ESM4::Hair* hair = store->get<ESM4::Hair>().search(traits.mHair))
                insertPart(hair->mModel);
            else
                Log(Debug::Error) << "Hair not found: " << ESM::RefId(traits.mHair);
        }

        for (const ESM4::Armor* armor : MWClass::ESM4Npc::getEquippedArmor(mPtr))
            insertPart(chooseTes4EquipmentModel(armor, isFemale));
        for (const ESM4::Clothing* clothing : MWClass::ESM4Npc::getEquippedClothing(mPtr))
            insertPart(chooseTes4EquipmentModel(clothing, isFemale));
    }

    void ESM4NpcAnimation::updatePartsFONV(const ESM4::Npc& traits)
    {
        const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
        const ESM4::Race* race = MWClass::ESM4Npc::getRace(mPtr);
        if (race == nullptr)
        {
            Log(Debug::Warning) << "FNV/ESM4: cannot assemble actor without race " << traits.mEditorId;
            return;
        }

        const bool isFemale = MWClass::ESM4Npc::isFemale(mPtr);
        unsigned int raceBodyCount = 0;
        unsigned int raceHeadCount = 0;
        unsigned int equipmentCount = 0;
        auto attach = [this](std::string_view model) {
            if (model.empty())
                return false;
            insertPart(model);
            return true;
        };

        // FO3/FNV RACE records contain the exposed body and face meshes.  The
        // upstream TES5 path intentionally ignores these legacy vectors, which
        // left every native Fallout NPC as a skeleton with no visible parts.
        const auto& bodyParts = isFemale ? race->mBodyPartsFemale : race->mBodyPartsMale;
        for (const ESM4::Race::BodyPart& bodyPart : bodyParts)
            if (attach(bodyPart.mesh))
                ++raceBodyCount;

        const auto& headParts = isFemale ? race->mHeadPartsFemale : race->mHeadParts;
        for (const ESM4::Race::BodyPart& headPart : headParts)
            if (attach(headPart.mesh))
                ++raceHeadCount;

        std::set<uint32_t> usedHeadPartTypes;
        insertHeadParts(traits.mHeadParts, usedHeadPartTypes);
        if (!traits.mHair.isZeroOrUnset() && usedHeadPartTypes.count(ESM4::HeadPart::Type_Hair) == 0)
        {
            if (const ESM4::Hair* hair = store->get<ESM4::Hair>().search(traits.mHair))
                attach(hair->mModel);
            else
                Log(Debug::Warning) << "FNV/ESM4: hair not found " << ESM::RefId(traits.mHair)
                                    << " for " << traits.mEditorId;
        }

        // Fallout armor points at a BIPL FormList of ARMA records.  Preserve
        // direct models too: some records use them as their complete biped
        // representation while others split the outfit into ARMA pieces.
        std::set<ESM::FormId> seenAddons;
        std::set<std::string> seenModels;
        const auto attachEquipment = [&](std::string_view model) {
            if (model.empty() || !seenModels.emplace(model).second)
                return;
            if (attach(model))
                ++equipmentCount;
        };
        for (const ESM4::Armor* armor : MWClass::ESM4Npc::getEquippedArmor(mPtr))
        {
            const std::string_view directModel
                = isFemale && !armor->mModelFemale.empty() ? armor->mModelFemale : armor->mModelMale;
            attachEquipment(directModel);

            std::vector<ESM::FormId> addonIds = armor->mAddOns;
            if (!armor->mBipedModelList.isZeroOrUnset())
            {
                if (const ESM4::FormIdList* list = store->get<ESM4::FormIdList>().search(armor->mBipedModelList))
                    addonIds.insert(addonIds.end(), list->mObjects.begin(), list->mObjects.end());
                else
                    Log(Debug::Warning) << "FNV/ESM4: BIPL list not found "
                                        << ESM::RefId(armor->mBipedModelList) << " for " << armor->mEditorId;
            }

            for (ESM::FormId addonId : addonIds)
            {
                if (addonId.isZeroOrUnset() || !seenAddons.emplace(addonId).second)
                    continue;
                const ESM4::ArmorAddon* addon = store->get<ESM4::ArmorAddon>().search(addonId);
                if (addon == nullptr)
                {
                    Log(Debug::Warning) << "FNV/ESM4: armor add-on not found " << ESM::RefId(addonId)
                                        << " for " << armor->mEditorId;
                    continue;
                }
                const bool compatible = (addon->mRacePrimary.isZeroOrUnset() && addon->mRaces.empty())
                    || addon->mRacePrimary == race->mId
                    || std::find(addon->mRaces.begin(), addon->mRaces.end(), race->mId) != addon->mRaces.end();
                if (!compatible)
                    continue;
                const std::string_view model
                    = isFemale && !addon->mModelFemale.empty() ? addon->mModelFemale : addon->mModelMale;
                attachEquipment(model);
            }
        }

        Log(Debug::Info) << "FNV/ESM4: assembled actor=" << traits.mEditorId
                         << " raceBody=" << raceBodyCount << " raceHead=" << raceHeadCount
                         << " equipment=" << equipmentCount;
    }

    void ESM4NpcAnimation::insertHeadParts(
        const std::vector<ESM::FormId>& partIds, std::set<uint32_t>& usedHeadPartTypes)
    {
        const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
        for (ESM::FormId partId : partIds)
        {
            if (partId.isZeroOrUnset())
                continue;
            const ESM4::HeadPart* part = store->get<ESM4::HeadPart>().search(partId);
            if (!part)
            {
                Log(Debug::Error) << "Head part not found: " << ESM::RefId(partId);
                continue;
            }
            if (usedHeadPartTypes.emplace(part->mType).second)
                insertPart(part->mModel);
        }
    }

    void ESM4NpcAnimation::updatePartsTES5(const ESM4::Npc& traits)
    {
        const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();

        const ESM4::Race* race = MWClass::ESM4Npc::getRace(mPtr);
        bool isFemale = MWClass::ESM4Npc::isFemale(mPtr);

        std::vector<const ESM4::ArmorAddon*> armorAddons;

        auto findArmorAddons = [&](const ESM4::Armor* armor) {
            for (ESM::FormId armaId : armor->mAddOns)
            {
                if (armaId.isZeroOrUnset())
                    continue;
                const ESM4::ArmorAddon* arma = store->get<ESM4::ArmorAddon>().search(armaId);
                if (!arma)
                {
                    Log(Debug::Error) << "ArmorAddon not found: " << ESM::RefId(armaId);
                    continue;
                }
                bool compatibleRace = arma->mRacePrimary == traits.mRace;
                for (auto r : arma->mRaces)
                    if (r == traits.mRace)
                        compatibleRace = true;
                if (compatibleRace)
                    armorAddons.push_back(arma);
            }
        };

        for (const ESM4::Armor* armor : MWClass::ESM4Npc::getEquippedArmor(mPtr))
            findArmorAddons(armor);
        if (!traits.mWornArmor.isZeroOrUnset())
        {
            if (const ESM4::Armor* armor = store->get<ESM4::Armor>().search(traits.mWornArmor))
                findArmorAddons(armor);
            else
                Log(Debug::Error) << "Worn armor not found: " << ESM::RefId(traits.mWornArmor);
        }
        if (!race->mSkin.isZeroOrUnset())
        {
            if (const ESM4::Armor* armor = store->get<ESM4::Armor>().search(race->mSkin))
                findArmorAddons(armor);
            else
                Log(Debug::Error) << "Skin not found: " << ESM::RefId(race->mSkin);
        }

        if (isFemale)
            std::sort(armorAddons.begin(), armorAddons.end(),
                [](auto x, auto y) { return x->mFemalePriority > y->mFemalePriority; });
        else
            std::sort(armorAddons.begin(), armorAddons.end(),
                [](auto x, auto y) { return x->mMalePriority > y->mMalePriority; });

        uint32_t usedParts = 0;
        for (const ESM4::ArmorAddon* arma : armorAddons)
        {
            const uint32_t covers = arma->mBodyTemplate.bodyPart;
            // if body is already covered, skip to avoid clipping
            if (covers & usedParts & ESM4::Armor::TES5_Body)
                continue;
            // if covers at least something that wasn't covered before - add model
            if (covers & ~usedParts)
            {
                usedParts |= covers;
                insertPart(isFemale ? arma->mModelFemale : arma->mModelMale);
            }
        }

        std::set<uint32_t> usedHeadPartTypes;
        if (usedParts & ESM4::Armor::TES5_Hair)
            usedHeadPartTypes.insert(ESM4::HeadPart::Type_Hair);
        insertHeadParts(traits.mHeadParts, usedHeadPartTypes);
        insertHeadParts(isFemale ? race->mHeadPartIdsFemale : race->mHeadPartIdsMale, usedHeadPartTypes);
    }
}
