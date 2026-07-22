#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <boost/program_options/options_description.hpp>
#include <boost/program_options/variables_map.hpp>

#include <components/esm/defs.hpp>
#include <components/esm/records.hpp>
#include <components/esm/typetraits.hpp>
#include <components/esm3/esmreader.hpp>
#include <components/esm3/esmwriter.hpp>
#include <components/esm3/readerscache.hpp>
#include <components/esm3/typetraits.hpp>
#include <components/esm4/common.hpp>
#include <components/esm4/loadclfm.hpp>
#include <components/esm4/loadclas.hpp>
#include <components/esm4/loadgmst.hpp>
#include <components/esm4/loadnpc.hpp>
#include <components/esm4/loadrace.hpp>
#include <components/esm4/reader.hpp>
#include <components/esm4/readerutils.hpp>
#include <components/esm4/records.hpp>
#include <components/files/configurationmanager.hpp>
#include <components/files/conversion.hpp>
#include <components/loadinglistener/loadinglistener.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/testing/util.hpp>

#include "apps/openmw/mwworld/esmstore.hpp"
#include "apps/openmw/mwworld/fnvplayerstate.hpp"

static Loading::Listener dummyListener;

namespace
{
    constexpr std::int32_t sFalloutMasterIndex = 37;

    constexpr ESM::FormId fnvForm(std::uint32_t index, std::int32_t contentFile = sFalloutMasterIndex)
    {
        return ESM::FormId{ index, contentFile };
    }

    template <class T>
    void appendPod(std::string& output, const T& value)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        output.append(reinterpret_cast<const char*>(&value), sizeof(value));
    }

    void appendSubRecord(std::string& output, std::string_view type, std::string_view data)
    {
        output.append(type);
        appendPod(output, static_cast<std::uint16_t>(data.size()));
        output.append(data);
    }

    std::unique_ptr<ESM4::Reader> makeHeaderOnlyReader(
        const std::filesystem::path& filename, std::uint32_t modIndex)
    {
        std::string hedr;
        appendPod(hedr, 1.34f);
        appendPod(hedr, std::int32_t{ 0 });
        appendPod(hedr, std::uint32_t{ 0x800 });

        std::string headerBody;
        appendSubRecord(headerBody, "HEDR", hedr);

        std::string plugin = "TES4";
        appendPod(plugin, static_cast<std::uint32_t>(headerBody.size()));
        appendPod(plugin, std::uint32_t{ 0 });
        appendPod(plugin, std::uint32_t{ 0 });
        appendPod(plugin, std::uint32_t{ 0 });
        appendPod(plugin, std::uint16_t{ 0 });
        appendPod(plugin, std::uint16_t{ 0 });
        plugin.append(headerBody);

        auto stream = std::make_unique<std::istringstream>(plugin, std::ios::in | std::ios::binary);
        auto reader = std::make_unique<ESM4::Reader>(std::move(stream), filename, nullptr, nullptr, true);
        reader->setModIndex(modIndex);
        return reader;
    }

    void observeNativeMaster(MWWorld::ESMStore& store, const std::filesystem::path& filename = "FalloutNV.esm",
        std::uint32_t modIndex = static_cast<std::uint32_t>(sFalloutMasterIndex))
    {
        std::unique_ptr<ESM4::Reader> reader = makeHeaderOnlyReader(filename, modIndex);
        store.loadESM4(*reader, nullptr);
    }

    ESM4::Npc makeNativePlayer(std::int32_t contentFile = sFalloutMasterIndex)
    {
        ESM4::Npc result{};
        result.mId = fnvForm(7, contentFile);
        result.mIsFONV = true;
        result.mEditorId = "Player";
        result.mFullName = "The Courier";
        result.mModel = "Characters\\_Male\\Skeleton.NIF";
        result.mRace = fnvForm(0x19, contentFile);
        result.mClass = fnvForm(0x57e6a, contentFile);
        result.mHasFNVBaseConfig = true;
        result.mBaseConfig.fo3.flags = ESM4::Npc::FO3_Female;
        result.mBaseConfig.fo3.fatigue = 201;
        result.mBaseConfig.fo3.levelOrMult = 7;
        result.mBaseConfig.fo3.templateFlags = 0;
        result.mHasFNVAIData = true;
        result.mHasFNVData = true;
        result.mFNVData = { 345, 1, 2, 3, 4, 5, 6, 7 };
        result.mHasFNVSkills = true;
        result.mFNVSkills.values = { 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24 };
        result.mFNVSkills.offsets = { 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44 };
        return result;
    }

    ESM4::Class makeNativePlayerClass(std::int32_t contentFile = sFalloutMasterIndex)
    {
        ESM4::Class result{};
        result.mId = fnvForm(0x57e6a, contentFile);
        result.mEditorId = "PlayerClass";
        result.mFullName = "Vault Dweller";
        result.mDesc = "Native player class";
        result.mHasFalloutData = true;
        result.mHasFalloutAttributes = true;
        result.mData.mRawFlags = 1;
        return result;
    }

    ESM4::Race makeNativePlayerRace(std::int32_t contentFile = sFalloutMasterIndex)
    {
        ESM4::Race result{};
        result.mId = fnvForm(0x19, contentFile);
        result.mEditorId = "Caucasian";
        result.mFullName = "Caucasian";
        result.mDesc = "Native player race";
        result.mHasFalloutData = true;
        result.mHeightMale = 1.01f;
        result.mHeightFemale = 0.97f;
        result.mWeightMale = 1.11f;
        result.mWeightFemale = 0.89f;
        result.mRaceFlags = 1;
        return result;
    }

    ESM4::GameSetting makeNativeGameSetting(
        std::uint32_t index, std::string editorId, ESM4::GameSetting::Data value)
    {
        ESM4::GameSetting result{};
        result.mId = fnvForm(index);
        result.mFlags = 0x400u;
        result.mEditorId = std::move(editorId);
        result.mData = std::move(value);
        return result;
    }

    enum class NativePlayerRecord
    {
        None,
        Npc,
        Class,
        Race,
    };

    void insertNativePlayerRecords(
        MWWorld::ESMStore& store, NativePlayerRecord omitted = NativePlayerRecord::None, bool insertDecoys = false)
    {
        if (omitted != NativePlayerRecord::Npc)
            store.insertStatic(makeNativePlayer());
        if (omitted != NativePlayerRecord::Class)
            store.insertStatic(makeNativePlayerClass());
        if (omitted != NativePlayerRecord::Race)
            store.insertStatic(makeNativePlayerRace());

        if (insertDecoys)
        {
            constexpr std::int32_t decoyNamespace = sFalloutMasterIndex + 1;
            store.insertStatic(makeNativePlayer(decoyNamespace));
            store.insertStatic(makeNativePlayerClass(decoyNamespace));
            store.insertStatic(makeNativePlayerRace(decoyNamespace));
        }
    }

    void validateStore(MWWorld::ESMStore& store)
    {
        ESM::ReadersCache readers;
        store.validateRecords(readers);
    }
}

TEST(FNVNativePlayerStore, validatedOfficialSetupUsesNativePlayerAndNeutralStructuralCarriers)
{
    MWWorld::ESMStore store;
    observeNativeMaster(store, "Data/FaLlOuTnV.EsM");
    insertNativePlayerRecords(store, NativePlayerRecord::None, true);
    store.insertStatic(makeNativeGameSetting(0x101, "fSwimHeightScale", 0.42f));
    store.insertStatic(makeNativeGameSetting(0x102, "sFNVBridgeProbe", std::string("authored")));

    ASSERT_NO_THROW(store.setUp());
    EXPECT_EQ(store.get<ESM4::GameSetting>().getSize(), 2u);
    EXPECT_FLOAT_EQ(store.get<ESM::GameSetting>().find("fSwimHeightScale")->mValue.getFloat(), 0.42f);
    EXPECT_EQ(store.get<ESM::GameSetting>().find("sFNVBridgeProbe")->mValue.getString(), "authored");
    EXPECT_EQ(store.get<ESM::GameSetting>().find("sDefaultCellname")->mValue.getString(), "Wasteland");
    EXPECT_EQ(store.get<ESM::GameSetting>().search("sAttributeStrength"), nullptr);
    EXPECT_EQ(store.get<ESM::GameSetting>().search("fWerewolfStrength"), nullptr);
    EXPECT_FLOAT_EQ(store.get<ESM::Global>().find(ESM::RefId::stringRefId("gamehour"))->mValue.getFloat(), 0.f);
    EXPECT_FLOAT_EQ(store.get<ESM::Global>().find(ESM::RefId::stringRefId("timescale"))->mValue.getFloat(), 30.f);
    EXPECT_EQ(store.get<ESM::Global>().find(ESM::RefId::stringRefId("year"))->mValue.getInteger(), 2281);

    const ESM::Attribute::AttributeID attributeIds[] = { ESM::Attribute::Strength, ESM::Attribute::Intelligence,
        ESM::Attribute::Willpower, ESM::Attribute::Agility, ESM::Attribute::Speed, ESM::Attribute::Endurance,
        ESM::Attribute::Personality, ESM::Attribute::Luck };
    ASSERT_EQ(store.get<ESM::Attribute>().getSize(), std::size(attributeIds));
    for (const ESM::Attribute::AttributeID& id : attributeIds)
    {
        const ESM::Attribute* attribute = store.get<ESM::Attribute>().search(ESM::RefId(id));
        ASSERT_NE(attribute, nullptr);
        EXPECT_TRUE(attribute->mName.empty());
        EXPECT_TRUE(attribute->mDescription.empty());
        EXPECT_TRUE(attribute->mIcon.empty());
        EXPECT_FLOAT_EQ(attribute->mWerewolfValue, 0.f);
    }

    ASSERT_EQ(store.get<ESM::Skill>().getSize(), ESM::Skill::Length);
    for (int index = 0; index < ESM::Skill::Length; ++index)
    {
        const ESM::RefId id = ESM::Skill::indexToRefId(index);
        const ESM::Skill* skill = store.get<ESM::Skill>().search(id);
        ASSERT_NE(skill, nullptr);
        EXPECT_TRUE(skill->mName.empty());
        EXPECT_TRUE(skill->mDescription.empty());
        EXPECT_TRUE(skill->mIcon.empty());
        EXPECT_FALSE(skill->mSchool.has_value());
        EXPECT_FLOAT_EQ(skill->mWerewolfValue, 0.f);
        EXPECT_THAT(skill->mData.mUseValue, testing::Each(0.f));
    }

    ASSERT_EQ(store.get<ESM::MagicEffect>().getSize(), ESM::MagicEffect::Length);
    for (int index = 0; index < ESM::MagicEffect::Length; ++index)
    {
        const ESM::MagicEffect* effect = store.get<ESM::MagicEffect>().search(index);
        ASSERT_NE(effect, nullptr);
        EXPECT_EQ(effect->mId, ESM::MagicEffect::indexToRefId(index));
        EXPECT_TRUE(effect->mDescription.empty());
        EXPECT_TRUE(effect->mIcon.empty());
        EXPECT_TRUE(effect->mParticle.empty());
        EXPECT_TRUE(effect->mCastSound.empty());
        EXPECT_TRUE(effect->mBoltSound.empty());
        EXPECT_TRUE(effect->mHitSound.empty());
        EXPECT_TRUE(effect->mAreaSound.empty());
    }

    ASSERT_NO_THROW(validateStore(store));
    ASSERT_EQ(store.getFalloutNewVegasMasterCandidateCount(), 1u);
    ASSERT_EQ(store.getFalloutNewVegasMasterCandidateIndex(), sFalloutMasterIndex);
    ASSERT_NE(store.getFalloutPlayerState(), nullptr);
    EXPECT_EQ(store.getFalloutPlayerState()->mBaseRecord, fnvForm(7));
    EXPECT_EQ(store.getFalloutPlayerState()->mReferenceRecord, fnvForm(0x14));
    EXPECT_EQ(store.get<ESM4::ActorCharacter>().search(fnvForm(0x14)), nullptr);

    const MWWorld::FalloutNativePlayerRecordsResolution native = store.getFalloutNativePlayerRecords();
    ASSERT_TRUE(native) << native.mError;
    EXPECT_EQ(native.mRecords->mBaseNpc, store.get<ESM4::Npc>().search(fnvForm(7)));
    EXPECT_EQ(native.mRecords->mClass, store.get<ESM4::Class>().search(fnvForm(0x57e6a)));
    EXPECT_EQ(native.mRecords->mRace, store.get<ESM4::Race>().search(fnvForm(0x19)));

    const ESM::RefId classId = ESM::RefId::formIdRefId(fnvForm(0x57e6a));
    const ESM::RefId raceId = ESM::RefId::formIdRefId(fnvForm(0x19));
    const ESM::RefId playerId = ESM::RefId::stringRefId("Player");
    const ESM::Class* playerClass = store.get<ESM::Class>().search(classId);
    const ESM::Race* playerRace = store.get<ESM::Race>().search(raceId);
    const ESM::NPC* player = store.get<ESM::NPC>().search(playerId);
    ASSERT_NE(playerClass, nullptr);
    ASSERT_NE(playerRace, nullptr);
    ASSERT_NE(player, nullptr);
    EXPECT_EQ(playerClass->mName, "Vault Dweller");
    EXPECT_EQ(playerRace->mName, "Caucasian");
    EXPECT_EQ(player->mRace, raceId);
    EXPECT_EQ(player->mClass, classId);
    EXPECT_EQ(player->mName, "The Courier");
    EXPECT_EQ(player->mModel, "Characters\\_Male\\Skeleton.NIF");
    EXPECT_EQ(player->mNpdt.mLevel, 7);
    EXPECT_EQ(player->mNpdt.mHealth, 345);
    EXPECT_EQ(player->mNpdt.mFatigue, 201);
    EXPECT_NE(player->mFlags & ESM::NPC::Female, 0);

    // The normal path must not receive the old proof-only inventory, topic, or synthetic class/race records.
    EXPECT_EQ(store.get<ESM::Weapon>().getSize(), 0u);
    EXPECT_EQ(store.get<ESM::Potion>().getSize(), 0u);
    EXPECT_EQ(store.get<ESM::Miscellaneous>().getSize(), 0u);
    EXPECT_EQ(store.get<ESM::Dialogue>().getSize(), 0u);
    EXPECT_EQ(store.get<ESM::Class>().search(ESM::RefId::stringRefId("FNV_Courier")), nullptr);
    EXPECT_EQ(store.get<ESM::Race>().search(ESM::RefId::stringRefId("FNV_Wastelander")), nullptr);

    ASSERT_NO_THROW(store.movePlayerRecord());
    EXPECT_EQ(store.get<ESM::NPC>().getDynamicSize(), 1);
    ASSERT_NO_THROW(store.clearDynamic());
    EXPECT_EQ(store.get<ESM::NPC>().getDynamicSize(), 1);
}

TEST(FNVNativePlayerStore, exactBasenameAloneCannotSelectOfficialSetup)
{
    MWWorld::ESMStore store;
    observeNativeMaster(store, "Elsewhere/FalloutNV.esm");

    EXPECT_EQ(store.getFalloutNewVegasMasterCandidateCount(), 1u);
    ASSERT_EQ(store.getFalloutNewVegasMasterCandidateIndex(), sFalloutMasterIndex);
    EXPECT_THROW(store.setUp(), std::runtime_error);
    EXPECT_EQ(store.getFalloutPlayerState(), nullptr);
    EXPECT_FALSE(store.getFalloutNativePlayerRecords());
    EXPECT_EQ(store.get<ESM::Class>().getSize(), 0u);
    EXPECT_EQ(store.get<ESM::Race>().getSize(), 0u);
    EXPECT_EQ(store.get<ESM::NPC>().getSize(), 0u);
}

TEST(FNVNativePlayerStore, rejectsMissingOrCorruptTypedIdentityBeforeChangingSetup)
{
    const NativePlayerRecord missingRecords[]{ NativePlayerRecord::Npc, NativePlayerRecord::Class,
        NativePlayerRecord::Race };
    for (const NativePlayerRecord missing : missingRecords)
    {
        SCOPED_TRACE(static_cast<int>(missing));
        MWWorld::ESMStore store;
        observeNativeMaster(store);
        insertNativePlayerRecords(store, missing, true);
        EXPECT_THROW(store.setUp(), std::runtime_error);
        EXPECT_EQ(store.getFalloutPlayerState(), nullptr);
        EXPECT_EQ(store.get<ESM::Attribute>().getSize(), 0u);
        EXPECT_EQ(store.get<ESM::Skill>().getSize(), 0u);
        EXPECT_EQ(store.get<ESM::MagicEffect>().getSize(), 0);
    }

    MWWorld::ESMStore corruptClassStore;
    observeNativeMaster(corruptClassStore);
    insertNativePlayerRecords(corruptClassStore);
    ESM4::Class corruptClass = makeNativePlayerClass();
    corruptClass.mHasFalloutAttributes = false;
    corruptClassStore.overrideRecord(corruptClass);
    EXPECT_THROW(corruptClassStore.setUp(), std::runtime_error);

    MWWorld::ESMStore corruptRaceStore;
    observeNativeMaster(corruptRaceStore);
    insertNativePlayerRecords(corruptRaceStore);
    ESM4::Race corruptRace = makeNativePlayerRace();
    corruptRace.mHasFalloutData = false;
    corruptRaceStore.overrideRecord(corruptRace);
    EXPECT_THROW(corruptRaceStore.setUp(), std::runtime_error);
}

TEST(FNVNativePlayerStore, rejectsAmbiguousOrUnrepresentableMasterCandidates)
{
    MWWorld::ESMStore duplicate;
    observeNativeMaster(duplicate, "Data/FalloutNV.esm");
    EXPECT_THROW(observeNativeMaster(duplicate, "Overrides/FALLOUTnv.ESM"), std::runtime_error);
    EXPECT_EQ(duplicate.getFalloutNewVegasMasterCandidateCount(), 2u);
    EXPECT_FALSE(duplicate.getFalloutNewVegasMasterCandidateIndex().has_value());

    MWWorld::ESMStore outOfRange;
    EXPECT_THROW(observeNativeMaster(outOfRange, "FalloutNV.esm", std::numeric_limits<std::uint32_t>::max()),
        std::runtime_error);
    EXPECT_EQ(outOfRange.getFalloutNewVegasMasterCandidateCount(), 1u);
    EXPECT_FALSE(outOfRange.getFalloutNewVegasMasterCandidateIndex().has_value());
}

/// Base class for tests of ESMStore that rely on external content files to produce the test results
struct ContentFileTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        readContentFiles();

        // load the content files
        int index = 0;
        ESM::Dialogue* dialogue = nullptr;
        for (const auto& mContentFile : mContentFiles)
        {
            ESM::ESMReader lEsm;
            lEsm.setEncoder(nullptr);
            lEsm.setIndex(index);
            lEsm.open(mContentFile);
            mEsmStore.load(lEsm, &dummyListener, dialogue);

            ++index;
        }

        mEsmStore.setUp();
    }

    void TearDown() override {}

    // read absolute path to content files from openmw.cfg
    void readContentFiles()
    {
        boost::program_options::variables_map variables;

        boost::program_options::options_description desc("Allowed options");
        auto addOption = desc.add_options();
        addOption("data",
            boost::program_options::value<Files::MaybeQuotedPathContainer>()
                ->default_value(Files::MaybeQuotedPathContainer(), "data")
                ->multitoken()
                ->composing());
        addOption("content",
            boost::program_options::value<std::vector<std::string>>()
                ->default_value(std::vector<std::string>(), "")
                ->multitoken()
                ->composing(),
            "content file(s): esm/esp, or omwgame/omwaddon");
        addOption("data-local",
            boost::program_options::value<Files::MaybeQuotedPathContainer::value_type>()->default_value(
                Files::MaybeQuotedPathContainer::value_type(), ""));
        Files::ConfigurationManager::addCommonOptions(desc);

        mConfigurationManager.readConfiguration(variables, desc, true);

        Files::PathContainer dataDirs, dataLocal;
        if (!variables["data"].empty())
        {
            dataDirs = asPathContainer(variables["data"].as<Files::MaybeQuotedPathContainer>());
        }

        Files::PathContainer::value_type local(
            variables["data-local"].as<Files::MaybeQuotedPathContainer::value_type>().u8string());
        if (!local.empty())
            dataLocal.push_back(local);

        mConfigurationManager.filterOutNonExistingPaths(dataDirs);
        mConfigurationManager.filterOutNonExistingPaths(dataLocal);

        if (!dataLocal.empty())
            dataDirs.insert(dataDirs.end(), dataLocal.begin(), dataLocal.end());

        Files::Collections collections(dataDirs);

        std::vector<std::string> contentFiles = variables["content"].as<std::vector<std::string>>();
        for (auto& contentFile : contentFiles)
        {
            if (!Misc::StringUtils::ciEndsWith(contentFile, ".omwscripts"))
                mContentFiles.push_back(collections.getPath(contentFile));
        }
    }

protected:
    Files::ConfigurationManager mConfigurationManager;
    MWWorld::ESMStore mEsmStore;
    std::vector<std::filesystem::path> mContentFiles;
};

/// Print results of the dialogue merging process, i.e. the resulting linked list.
TEST_F(ContentFileTest, dialogue_merging_test)
{
    if (mContentFiles.empty())
    {
        std::cout << "No content files found, skipping test" << std::endl;
        return;
    }

    const auto file = TestingOpenMW::outputFilePath("test_dialogue_merging.txt");
    std::ofstream stream(file);

    const MWWorld::Store<ESM::Dialogue>& dialStore = mEsmStore.get<ESM::Dialogue>();
    for (const auto& dial : dialStore)
    {
        stream << "Dialogue: " << dial.mId << std::endl;

        for (const auto& info : dial.mInfo)
        {
            stream << info.mId << std::endl;
        }
        stream << std::endl;
    }

    std::cout << "dialogue_merging_test successful, results printed to " << Files::pathToUnicodeString(file)
              << std::endl;
}

// Note: here we don't test records that don't use string names (e.g. Land, Pathgrid, Cell)
#define RUN_TEST_FOR_TYPES(func, arg1, arg2)                                                                           \
    func<ESM::Activator>(arg1, arg2);                                                                                  \
    func<ESM::Apparatus>(arg1, arg2);                                                                                  \
    func<ESM::Armor>(arg1, arg2);                                                                                      \
    func<ESM::BirthSign>(arg1, arg2);                                                                                  \
    func<ESM::BodyPart>(arg1, arg2);                                                                                   \
    func<ESM::Book>(arg1, arg2);                                                                                       \
    func<ESM::Class>(arg1, arg2);                                                                                      \
    func<ESM::Clothing>(arg1, arg2);                                                                                   \
    func<ESM::Container>(arg1, arg2);                                                                                  \
    func<ESM::Creature>(arg1, arg2);                                                                                   \
    func<ESM::CreatureLevList>(arg1, arg2);                                                                            \
    func<ESM::Dialogue>(arg1, arg2);                                                                                   \
    func<ESM::Door>(arg1, arg2);                                                                                       \
    func<ESM::Enchantment>(arg1, arg2);                                                                                \
    func<ESM::Faction>(arg1, arg2);                                                                                    \
    func<ESM::GameSetting>(arg1, arg2);                                                                                \
    func<ESM::Global>(arg1, arg2);                                                                                     \
    func<ESM::Ingredient>(arg1, arg2);                                                                                 \
    func<ESM::ItemLevList>(arg1, arg2);                                                                                \
    func<ESM::Light>(arg1, arg2);                                                                                      \
    func<ESM::Lockpick>(arg1, arg2);                                                                                   \
    func<ESM::Miscellaneous>(arg1, arg2);                                                                              \
    func<ESM::NPC>(arg1, arg2);                                                                                        \
    func<ESM::Potion>(arg1, arg2);                                                                                     \
    func<ESM::Probe>(arg1, arg2);                                                                                      \
    func<ESM::Race>(arg1, arg2);                                                                                       \
    func<ESM::Region>(arg1, arg2);                                                                                     \
    func<ESM::Repair>(arg1, arg2);                                                                                     \
    func<ESM::Script>(arg1, arg2);                                                                                     \
    func<ESM::Sound>(arg1, arg2);                                                                                      \
    func<ESM::SoundGenerator>(arg1, arg2);                                                                             \
    func<ESM::Spell>(arg1, arg2);                                                                                      \
    func<ESM::StartScript>(arg1, arg2);                                                                                \
    func<ESM::Weapon>(arg1, arg2);

template <typename T>
void printRecords(MWWorld::ESMStore& esmStore, std::ostream& outStream)
{
    const MWWorld::Store<T>& store = esmStore.get<T>();
    outStream << store.getSize() << " " << T::getRecordType() << " records" << std::endl;

    for (typename MWWorld::Store<T>::iterator it = store.begin(); it != store.end(); ++it)
    {
        const T& record = *it;
        outStream << record.mId << std::endl;
    }

    outStream << std::endl;
}

/// Print some basic diagnostics about the loaded content files, e.g. number of records and names of those records
/// Also used to test the iteration order of records
TEST_F(ContentFileTest, content_diagnostics_test)
{
    if (mContentFiles.empty())
    {
        std::cout << "No content files found, skipping test" << std::endl;
        return;
    }

    const auto file = TestingOpenMW::outputFilePath("test_content_diagnostics.txt");
    std::ofstream stream(file);

    RUN_TEST_FOR_TYPES(printRecords, mEsmStore, stream);

    std::cout << "diagnostics_test successful, results printed to " << Files::pathToUnicodeString(file) << std::endl;
}

// TODO:
/// Print results of autocalculated NPC spell lists. Also serves as test for attribute/skill autocalculation which the
/// spell autocalculation heavily relies on
/// - even incorrect rounding modes can completely change the resulting spell lists.
/*
TEST_F(ContentFileTest, autocalc_test)
{
    if (mContentFiles.empty())
    {
        std::cout << "No content files found, skipping test" << std::endl;
        return;
    }


}
*/

/// Base class for tests of ESMStore that do not rely on external content files
template <class T>
struct StoreTest : public ::testing::Test
{
};

TYPED_TEST_SUITE_P(StoreTest);

/// Create an ESM file in-memory containing the specified record.
/// @param deleted Write record with deleted flag?
template <typename T>
std::unique_ptr<std::istream> getEsmFile(T record, bool deleted, ESM::FormatVersion formatVersion)
{
    ESM::ESMWriter writer;
    auto stream = std::make_unique<std::stringstream>();
    writer.setFormatVersion(formatVersion);
    writer.save(*stream);
    writer.startRecord(T::sRecordId);
    record.save(writer, deleted);
    writer.endRecord(T::sRecordId);

    return stream;
}

namespace
{
    std::vector<ESM::FormatVersion> getFormats()
    {
        std::vector<ESM::FormatVersion> result({
            ESM::DefaultFormatVersion,
            ESM::CurrentContentFormatVersion,
            ESM::MaxOldFogOfWarFormatVersion,
            ESM::MaxUnoptimizedCharacterDataFormatVersion,
            ESM::MaxOldTimeLeftFormatVersion,
            ESM::MaxIntFallbackFormatVersion,
            ESM::MaxClearModifiersFormatVersion,
            ESM::MaxOldAiPackageFormatVersion,
            ESM::MaxOldSkillsAndAttributesFormatVersion,
            ESM::MaxOldCreatureStatsFormatVersion,
            ESM::MaxStringRefIdFormatVersion,
            ESM::MaxUseEsmCellIdFormatVersion,
        });
        for (ESM::FormatVersion v = result.back() + 1; v <= ESM::CurrentSaveGameFormatVersion; ++v)
            result.push_back(v);
        return result;
    }

    template <class T, class = std::void_t<>>
    struct HasBlankFunction : std::false_type
    {
    };

    template <class T>
    struct HasBlankFunction<T, std::void_t<decltype(std::declval<T>().blank())>> : std::true_type
    {
    };

    template <class T>
    constexpr bool hasBlankFunction = HasBlankFunction<T>::value;
}

/// Tests deletion of records.
TYPED_TEST_P(StoreTest, delete_test)
{
    using RecordType = TypeParam;

    for (const ESM::FormatVersion formatVersion : getFormats())
    {
        SCOPED_TRACE("FormatVersion: " + std::to_string(formatVersion));
        const ESM::RefId recordId = ESM::RefId::stringRefId("foobar");

        RecordType record;
        if constexpr (hasBlankFunction<RecordType>)
            record.blank();
        record.mId = recordId;

        ESM::ESMReader reader;
        ESM::Dialogue* dialogue = nullptr;

        {
            MWWorld::ESMStore esmStore;
            reader.open(getEsmFile(record, false, formatVersion), "filename");
            esmStore.load(reader, &dummyListener, dialogue); // master file inserts a record
            esmStore.setUp();

            EXPECT_EQ(esmStore.get<RecordType>().getSize(), 1);
        }
        {
            MWWorld::ESMStore esmStore;
            reader.open(getEsmFile(record, false, formatVersion), "filename");
            esmStore.load(reader, &dummyListener, dialogue); // master file inserts a record
            reader.open(getEsmFile(record, true, formatVersion), "filename");
            esmStore.load(reader, &dummyListener, dialogue); // now a plugin deletes it
            esmStore.setUp();

            EXPECT_EQ(esmStore.get<RecordType>().getSize(), 0);
        }
        {
            MWWorld::ESMStore esmStore;
            reader.open(getEsmFile(record, false, formatVersion), "filename");
            esmStore.load(reader, &dummyListener, dialogue); // master file inserts a record
            reader.open(getEsmFile(record, true, formatVersion), "filename");
            esmStore.load(reader, &dummyListener, dialogue); // now a plugin deletes it
            // now another plugin inserts it again
            // expected behaviour is the record to reappear rather than staying deleted
            reader.open(getEsmFile(record, false, formatVersion), "filename");
            esmStore.load(reader, &dummyListener, dialogue);
            esmStore.setUp();

            EXPECT_EQ(esmStore.get<RecordType>().getSize(), 1);
        }
    }
}

template <typename T>
static unsigned int hasSameRecordId(const MWWorld::Store<T>& store, ESM::RecNameInts recName)
{
    if constexpr (MWWorld::HasRecordId<T>::value)
    {
        return T::sRecordId == recName ? 1 : 0;
    }
    else
    {
        return 0;
    }
}

template <typename T>
static void testRecNameIntCount(const MWWorld::Store<T>& store, const MWWorld::ESMStore::StoreTuple& stores)
{
    if constexpr (MWWorld::HasRecordId<T>::value)
    {
        const unsigned int recordIdCount
            = std::apply([](auto&&... x) { return (hasSameRecordId(x, T::sRecordId) + ...); }, stores);
        ASSERT_EQ(recordIdCount, static_cast<unsigned int>(1))
            << "The same RecNameInt is used twice ESM::REC_" << ESM::getRecNameString(T::sRecordId).toStringView();
    }
}

static void testAllRecNameIntUnique(const MWWorld::ESMStore::StoreTuple& stores)
{
    std::apply([&stores](auto&&... x) { (testRecNameIntCount(x, stores), ...); }, stores);
}

TEST(StoreTest, eachRecordTypeShouldHaveUniqueRecordId)
{
    testAllRecNameIntUnique(MWWorld::ESMStore::StoreTuple());
}

/// Tests overwriting of records.
TYPED_TEST_P(StoreTest, overwrite_test)
{
    using RecordType = TypeParam;

    for (const ESM::FormatVersion formatVersion : getFormats())
    {
        SCOPED_TRACE("FormatVersion: " + std::to_string(formatVersion));

        const ESM::RefId recordId = ESM::RefId::stringRefId("foobar");
        const ESM::RefId recordIdUpper = ESM::RefId::stringRefId("Foobar");

        RecordType record;
        if constexpr (hasBlankFunction<RecordType>)
            record.blank();
        record.mId = recordId;

        ESM::ESMReader reader;
        ESM::Dialogue* dialogue = nullptr;
        MWWorld::ESMStore esmStore;

        // master file inserts a record
        reader.open(getEsmFile(record, false, formatVersion), "filename");
        esmStore.load(reader, &dummyListener, dialogue);

        // now a plugin overwrites it with changed data
        record.mId = recordIdUpper; // change id to uppercase, to test case smashing while we're at it
        record.mModel = "the_new_model";
        reader.open(getEsmFile(record, false, formatVersion), "filename");
        esmStore.load(reader, &dummyListener, dialogue);

        esmStore.setUp();

        // verify that changes were actually applied
        const RecordType* overwrittenRec = esmStore.get<RecordType>().search(recordId);

        ASSERT_NE(overwrittenRec, nullptr);

        EXPECT_EQ(overwrittenRec->mModel, "the_new_model");
    }
}

namespace
{
    using namespace ::testing;

    template <class T>
    struct StoreSaveLoadTest : public Test
    {
    };

    TYPED_TEST_SUITE_P(StoreSaveLoadTest);

    TYPED_TEST_P(StoreSaveLoadTest, shouldNotChangeRefId)
    {
        using RecordType = TypeParam;

        const int index = 3;
        const std::string stringId = "foobar";
        decltype(RecordType::mId) refId;
        if constexpr (ESM::hasIndex<RecordType> && !std::is_same_v<RecordType, ESM::LandTexture>)
            refId = RecordType::indexToRefId(index);
        else if constexpr (std::is_same_v<RecordType, ESM::Cell>)
        {
            refId = ESM::RefId::esm3ExteriorCell(0, 0);
        }
        else if constexpr (std::is_same_v<RecordType, ESM::Attribute>)
            refId = ESM::Attribute::Strength;
        else if constexpr (std::is_same_v<RecordType, ESM::Skill>)
            refId = ESM::Skill::Block;
        else
            refId = ESM::StringRefId(stringId);

        for (const ESM::FormatVersion formatVersion : getFormats())
        {
            SCOPED_TRACE("FormatVersion: " + std::to_string(formatVersion));

            RecordType record;

            if constexpr (hasBlankFunction<RecordType>)
                record.blank();

            record.mId = refId;

            if constexpr (ESM::hasStringId<RecordType>)
                record.mStringId = stringId;

            if constexpr (ESM::hasIndex<RecordType>)
                record.mIndex = index;

            if constexpr (std::is_same_v<RecordType, ESM::Global>)
                record.mValue = ESM::Variant(42);

            ESM::ESMReader reader;
            ESM::Dialogue* dialogue = nullptr;
            MWWorld::ESMStore esmStore;

            if constexpr (std::is_same_v<RecordType, ESM::Attribute>)
            {
                ASSERT_ANY_THROW(getEsmFile(record, false, formatVersion));
                continue;
            }

            reader.open(getEsmFile(record, false, formatVersion), "filename");
            ASSERT_NO_THROW(esmStore.load(reader, &dummyListener, dialogue));
            esmStore.setUp();

            const RecordType* result = nullptr;
            if constexpr (std::is_same_v<RecordType, ESM::LandTexture>)
            {
                const std::string* texture = esmStore.get<RecordType>().search(index, 0);
                ASSERT_NE(texture, nullptr);
                return;
            }
            else if constexpr (ESM::hasIndex<RecordType>)
                result = esmStore.get<RecordType>().search(index);
            else
                result = esmStore.get<RecordType>().search(refId);

            ASSERT_NE(result, nullptr);
            EXPECT_EQ(result->mId, refId);
        }
    }

    static_assert(ESM::hasIndex<ESM::MagicEffect>);
    static_assert(ESM::hasStringId<ESM::Dialogue>);

    template <class T, class = std::void_t<>>
    struct HasSaveFunction : std::false_type
    {
    };

    template <class T>
    struct HasSaveFunction<T, std::void_t<decltype(std::declval<T>().save(std::declval<ESM::ESMWriter&>(), bool()))>>
        : std::true_type
    {
    };

    template <class Head, class List>
    struct ConcatTypes;

    template <class Head, class... Ts>
    struct ConcatTypes<Head, std::tuple<Ts...>>
    {
        using Type = std::tuple<Head, Ts...>;
    };

    template <template <class...> class Predicate, class Out, class... Ins>
    struct FilterTypesImpl;

    template <template <class...> class Predicate, class Out, class Head, class... Tail>
    struct FilterTypesImpl<Predicate, Out, Head, Tail...>
    {
        using Type = typename FilterTypesImpl<Predicate,
            std::conditional_t<Predicate<Head>::value, typename ConcatTypes<Head, Out>::Type, Out>, Tail...>::Type;
    };

    template <template <class...> class Predicate, class Out>
    struct FilterTypesImpl<Predicate, Out>
    {
        using Type = Out;
    };

    template <template <class...> class Predicate, class List>
    struct FilterTypes;

    template <template <class...> class Predicate, class... Ts>
    struct FilterTypes<Predicate, std::tuple<Ts...>>
    {
        using Type = typename FilterTypesImpl<Predicate, std::tuple<>, Ts...>::Type;
    };

    template <class... T>
    struct ToRecordTypes;

    template <class... T>
    struct ToRecordTypes<std::tuple<MWWorld::Store<T>...>>
    {
        using Type = std::tuple<T...>;
    };

    template <class... T>
    struct AsTestingTypes;

    template <class... T>
    struct AsTestingTypes<std::tuple<T...>>
    {
        using Type = Types<T...>;
    };

    using RecordTypes = typename ToRecordTypes<MWWorld::ESMStore::StoreTuple>::Type;
    using RecordTypesWithId = typename FilterTypes<ESM::HasId, RecordTypes>::Type;
    using RecordTypesWithSave = typename FilterTypes<HasSaveFunction, RecordTypesWithId>::Type;
    using RecordTypesWithModel = typename FilterTypes<ESM::HasModel, RecordTypesWithSave>::Type;

    REGISTER_TYPED_TEST_SUITE_P(StoreSaveLoadTest, shouldNotChangeRefId);

    static_assert(std::tuple_size_v<RecordTypesWithSave> == 40);

    INSTANTIATE_TYPED_TEST_SUITE_P(
        RecordTypesTest, StoreSaveLoadTest, typename AsTestingTypes<RecordTypesWithSave>::Type);
}

REGISTER_TYPED_TEST_SUITE_P(StoreTest, overwrite_test, delete_test);

static_assert(std::tuple_size_v<RecordTypesWithModel> == 19);

INSTANTIATE_TYPED_TEST_SUITE_P(RecordTypesTest, StoreTest, typename AsTestingTypes<RecordTypesWithModel>::Type);

namespace ESM
{
    inline std::ostream& operator<<(std::ostream& stream, const ESM::DialInfo& value)
    {
        return stream << "ESM::DialInfo{.mId = " << value.mId << "}";
    }
}

namespace
{
    using namespace ::testing;

    struct DialogueData
    {
        ESM::Dialogue mDialogue;
        std::vector<ESM::DialInfo> mInfos;
    };

    DialogueData generateDialogueWithInfos(std::size_t infoCount)
    {
        DialogueData result;

        result.mDialogue.blank();
        result.mDialogue.mId = ESM::RefId::stringRefId("dialogue");
        result.mDialogue.mStringId = "Dialogue";

        for (std::size_t i = 0; i < infoCount; ++i)
        {
            ESM::DialInfo& info = result.mInfos.emplace_back();
            info.blank();
            info.mId = ESM::RefId::stringRefId("info" + std::to_string(i));
        }

        if (infoCount >= 2)
        {
            result.mInfos[0].mNext = result.mInfos[1].mId;
            result.mInfos[infoCount - 1].mPrev = result.mInfos[infoCount - 2].mId;
        }

        for (std::size_t i = 1; i < infoCount - 1; ++i)
        {
            result.mInfos[i].mPrev = result.mInfos[i - 1].mId;
            result.mInfos[i].mNext = result.mInfos[i + 1].mId;
        }

        return result;
    }

    std::unique_ptr<std::stringstream> saveDialogueWithInfos(
        const ESM::Dialogue& dialogue, std::span<const ESM::DialInfo> infos, std::span<const std::size_t> deleted = {})
    {
        auto stream = std::make_unique<std::stringstream>();

        ESM::ESMWriter writer;
        writer.setFormatVersion(ESM::CurrentSaveGameFormatVersion);
        writer.save(*stream);

        writer.startRecord(ESM::REC_DIAL);
        dialogue.save(writer);
        writer.endRecord(ESM::REC_DIAL);

        for (std::size_t i = 0; i < infos.size(); ++i)
        {
            writer.startRecord(ESM::REC_INFO);
            infos[i].save(writer, std::find(deleted.begin(), deleted.end(), i) != deleted.end());
            writer.endRecord(ESM::REC_INFO);
        }

        return stream;
    }

    void loadEsmStore(int index, std::unique_ptr<std::istream>&& stream, MWWorld::ESMStore& esmStore)
    {
        ESM::ESMReader reader;
        ESM::Dialogue* dialogue = nullptr;

        reader.setIndex(index);
        reader.open(std::move(stream), "test");
        esmStore.load(reader, &dummyListener, dialogue);
    }

    MATCHER_P(HasIdEqualTo, v, "")
    {
        return v == arg.mId;
    }

    TEST(MWWorldStoreTest, shouldLoadDialogueWithInfos)
    {
        const DialogueData data = generateDialogueWithInfos(3);

        MWWorld::ESMStore esmStore;
        loadEsmStore(0, saveDialogueWithInfos(data.mDialogue, data.mInfos), esmStore);
        esmStore.setUp();

        const ESM::Dialogue* dialogue = esmStore.get<ESM::Dialogue>().search(ESM::RefId::stringRefId("dialogue"));
        ASSERT_NE(dialogue, nullptr);
        EXPECT_THAT(dialogue->mInfo, ElementsAre(HasIdEqualTo("info0"), HasIdEqualTo("info1"), HasIdEqualTo("info2")));
    }

    TEST(MWWorldStoreTest, shouldIgnoreNextWhenLoadingDialogueInfos)
    {
        DialogueData data = generateDialogueWithInfos(3);

        std::reverse(data.mInfos.begin(), data.mInfos.end());

        MWWorld::ESMStore esmStore;
        loadEsmStore(0, saveDialogueWithInfos(data.mDialogue, data.mInfos), esmStore);
        esmStore.setUp();

        const ESM::Dialogue* dialogue = esmStore.get<ESM::Dialogue>().search(ESM::RefId::stringRefId("dialogue"));
        ASSERT_NE(dialogue, nullptr);
        EXPECT_THAT(dialogue->mInfo, ElementsAre(HasIdEqualTo("info0"), HasIdEqualTo("info2"), HasIdEqualTo("info1")));
    }

    TEST(MWWorldStoreTest, shouldLoadDialogueWithInfosInsertingNewRecordBasedOnPrev)
    {
        const DialogueData data = generateDialogueWithInfos(3);

        MWWorld::ESMStore esmStore;
        loadEsmStore(0, saveDialogueWithInfos(data.mDialogue, data.mInfos), esmStore);

        ESM::DialInfo newInfo;
        newInfo.blank();
        newInfo.mId = ESM::RefId::stringRefId("newInfo");
        newInfo.mPrev = data.mInfos[1].mId;
        newInfo.mNext = ESM::RefId::stringRefId("invalid");

        loadEsmStore(1, saveDialogueWithInfos(data.mDialogue, std::array{ newInfo }), esmStore);

        esmStore.setUp();

        const ESM::Dialogue* dialogue = esmStore.get<ESM::Dialogue>().search(ESM::RefId::stringRefId("dialogue"));
        ASSERT_NE(dialogue, nullptr);
        EXPECT_THAT(dialogue->mInfo,
            ElementsAre(HasIdEqualTo("info0"), HasIdEqualTo("info1"), HasIdEqualTo("newInfo"), HasIdEqualTo("info2")));
    }

    TEST(MWWorldStoreTest, shouldLoadDialogueWithInfosInsertingNewRecordToFrontBasedOnEmptyPrev)
    {
        const DialogueData data = generateDialogueWithInfos(3);

        MWWorld::ESMStore esmStore;
        loadEsmStore(0, saveDialogueWithInfos(data.mDialogue, data.mInfos), esmStore);

        ESM::DialInfo newInfo;
        newInfo.blank();
        newInfo.mId = ESM::RefId::stringRefId("newInfo");
        newInfo.mNext = ESM::RefId::stringRefId("invalid");

        loadEsmStore(1, saveDialogueWithInfos(data.mDialogue, std::array{ newInfo }), esmStore);

        esmStore.setUp();

        const ESM::Dialogue* dialogue = esmStore.get<ESM::Dialogue>().search(ESM::RefId::stringRefId("dialogue"));
        ASSERT_NE(dialogue, nullptr);
        EXPECT_THAT(dialogue->mInfo,
            ElementsAre(HasIdEqualTo("newInfo"), HasIdEqualTo("info0"), HasIdEqualTo("info1"), HasIdEqualTo("info2")));
    }

    TEST(MWWorldStoreTest, shouldLoadDialogueWithInfosInsertingNewRecordToBackWhenPrevIsNotFound)
    {
        const DialogueData data = generateDialogueWithInfos(3);

        MWWorld::ESMStore esmStore;
        loadEsmStore(0, saveDialogueWithInfos(data.mDialogue, data.mInfos), esmStore);

        ESM::DialInfo newInfo;
        newInfo.blank();
        newInfo.mId = ESM::RefId::stringRefId("newInfo");
        newInfo.mPrev = ESM::RefId::stringRefId("invalid");

        loadEsmStore(1, saveDialogueWithInfos(data.mDialogue, std::array{ newInfo }), esmStore);

        esmStore.setUp();

        const ESM::Dialogue* dialogue = esmStore.get<ESM::Dialogue>().search(ESM::RefId::stringRefId("dialogue"));
        ASSERT_NE(dialogue, nullptr);
        EXPECT_THAT(dialogue->mInfo,
            ElementsAre(HasIdEqualTo("info0"), HasIdEqualTo("info1"), HasIdEqualTo("info2"), HasIdEqualTo("newInfo")));
    }

    TEST(MWWorldStoreTest, shouldLoadDialogueWithInfosUpdatingExistingRecord)
    {
        const DialogueData data = generateDialogueWithInfos(3);

        MWWorld::ESMStore esmStore;
        loadEsmStore(0, saveDialogueWithInfos(data.mDialogue, data.mInfos), esmStore);

        ESM::DialInfo updatedInfo = data.mInfos[1];
        updatedInfo.mActor = ESM::RefId::stringRefId("newActor");

        loadEsmStore(1, saveDialogueWithInfos(data.mDialogue, std::array{ updatedInfo }), esmStore);

        esmStore.setUp();

        const ESM::Dialogue* dialogue = esmStore.get<ESM::Dialogue>().search(ESM::RefId::stringRefId("dialogue"));
        ASSERT_NE(dialogue, nullptr);
        ASSERT_EQ(dialogue->mInfo.size(), 3);
        EXPECT_EQ(std::next(dialogue->mInfo.begin())->mActor, "newActor");
    }

    TEST(MWWorldStoreTest, shouldLoadDialogueWithInfosMovingForwardExistingRecordBasedOnPrev)
    {
        const DialogueData data = generateDialogueWithInfos(3);

        MWWorld::ESMStore esmStore;
        loadEsmStore(0, saveDialogueWithInfos(data.mDialogue, data.mInfos), esmStore);

        ESM::DialInfo updatedInfo = data.mInfos[0];
        updatedInfo.mPrev = data.mInfos[2].mId;
        updatedInfo.mActor = ESM::RefId::stringRefId("newActor");

        loadEsmStore(1, saveDialogueWithInfos(data.mDialogue, std::array{ updatedInfo }), esmStore);

        esmStore.setUp();

        const ESM::Dialogue* dialogue = esmStore.get<ESM::Dialogue>().search(ESM::RefId::stringRefId("dialogue"));
        ASSERT_NE(dialogue, nullptr);
        EXPECT_THAT(dialogue->mInfo, ElementsAre(HasIdEqualTo("info1"), HasIdEqualTo("info2"), HasIdEqualTo("info0")));
        EXPECT_EQ(std::prev(dialogue->mInfo.end())->mActor, "newActor");
    }

    TEST(MWWorldStoreTest, shouldLoadDialogueWithInfosMovingBackwardExistingRecordBasedOnPrev)
    {
        const DialogueData data = generateDialogueWithInfos(3);

        MWWorld::ESMStore esmStore;
        loadEsmStore(0, saveDialogueWithInfos(data.mDialogue, data.mInfos), esmStore);

        ESM::DialInfo updatedInfo = data.mInfos[2];
        updatedInfo.mPrev = data.mInfos[0].mId;
        updatedInfo.mActor = ESM::RefId::stringRefId("newActor");

        loadEsmStore(1, saveDialogueWithInfos(data.mDialogue, std::array{ updatedInfo }), esmStore);

        esmStore.setUp();

        const ESM::Dialogue* dialogue = esmStore.get<ESM::Dialogue>().search(ESM::RefId::stringRefId("dialogue"));
        ASSERT_NE(dialogue, nullptr);
        EXPECT_THAT(dialogue->mInfo, ElementsAre(HasIdEqualTo("info0"), HasIdEqualTo("info2"), HasIdEqualTo("info1")));
        EXPECT_EQ(std::next(dialogue->mInfo.begin())->mActor, "newActor");
    }

    TEST(MWWorldStoreTest, shouldPreservePositionWhenPrevIsTheSame)
    {
        const DialogueData data = generateDialogueWithInfos(3);

        MWWorld::ESMStore esmStore;
        loadEsmStore(0, saveDialogueWithInfos(data.mDialogue, data.mInfos), esmStore);

        ESM::DialInfo newInfo = data.mInfos[0];
        newInfo.mPrev = data.mInfos[2].mId;
        newInfo.mNext = {};

        loadEsmStore(1, saveDialogueWithInfos(data.mDialogue, std::array{ newInfo }), esmStore);

        newInfo = data.mInfos[1];
        newInfo.mResponse = "test";

        loadEsmStore(2, saveDialogueWithInfos(data.mDialogue, std::array{ newInfo }), esmStore);

        esmStore.setUp();

        const ESM::Dialogue* dialogue = esmStore.get<ESM::Dialogue>().search(ESM::RefId::stringRefId("dialogue"));
        ASSERT_NE(dialogue, nullptr);
        EXPECT_THAT(dialogue->mInfo, ElementsAre(HasIdEqualTo("info1"), HasIdEqualTo("info2"), HasIdEqualTo("info0")));
        EXPECT_EQ(dialogue->mInfo.begin()->mResponse, "test");
    }

    TEST(MWWorldStoreTest, setUpShouldRemoveDeletedDialogueInfos)
    {
        const DialogueData data = generateDialogueWithInfos(3);

        MWWorld::ESMStore esmStore;
        const std::array<std::size_t, 1> deleted = { 1 };
        loadEsmStore(0, saveDialogueWithInfos(data.mDialogue, data.mInfos, deleted), esmStore);
        esmStore.setUp();

        const ESM::Dialogue* dialogue = esmStore.get<ESM::Dialogue>().search(ESM::RefId::stringRefId("dialogue"));
        ASSERT_NE(dialogue, nullptr);
        EXPECT_THAT(dialogue->mInfo, ElementsAre(HasIdEqualTo("info0"), HasIdEqualTo("info2")));
    }

    TEST(MWWorldStoreTest, shouldIndexEsm4ClothingAndKeys)
    {
        ESM4::Clothing clothing;
        clothing.mId = ESM::FormId::fromUint32(0x01000001);

        ESM4::Key key;
        key.mId = ESM::FormId::fromUint32(0x01000002);

        MWWorld::ESMStore store;
        auto& clothingStore = const_cast<MWWorld::Store<ESM4::Clothing>&>(store.get<ESM4::Clothing>());
        auto& keyStore = const_cast<MWWorld::Store<ESM4::Key>&>(store.get<ESM4::Key>());
        clothingStore.insertStatic(clothing);
        keyStore.insertStatic(key);
        store.setUp();

        const ESM::RefId clothingId = ESM::RefId::formIdRefId(clothing.mId);
        const ESM::RefId keyId = ESM::RefId::formIdRefId(key.mId);

        EXPECT_EQ(store.find(clothingId), ESM::REC_CLOT4);
        EXPECT_EQ(store.findStatic(clothingId), ESM::REC_CLOT4);
        EXPECT_EQ(store.find(keyId), ESM::REC_KEYM4);
        EXPECT_EQ(store.findStatic(keyId), ESM::REC_KEYM4);
    }
}
