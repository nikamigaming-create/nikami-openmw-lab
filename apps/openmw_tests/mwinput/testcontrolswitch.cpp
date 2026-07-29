#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string_view>

#include <components/esm/defs.hpp>
#include <components/esm3/esmreader.hpp>
#include <components/esm3/esmwriter.hpp>
#include <components/loadinglistener/loadinglistener.hpp>

#include "apps/openmw/mwinput/controlswitch.hpp"

TEST(ControlSwitchTest, PersistsIndependentMovementInterfaceSneakingAndRolloverSwitches)
{
    MWInput::ControlSwitch controls;
    for (const std::string_view key :
        { "playercontrols", "playermovement", "playerinterface", "playerfighting",
            "playerviewswitch", "playerlooking", "playersneaking", "playerrollover" })
        EXPECT_TRUE(controls.get(key)) << key;

    controls.set("playermovement", false);
    controls.set("playerinterface", false);
    controls.set("playersneaking", false);
    controls.set("playerrollover", false);
    EXPECT_TRUE(controls.get("playercontrols"));
    EXPECT_TRUE(controls.get("playerfighting"));
    EXPECT_TRUE(controls.get("playerviewswitch"));
    EXPECT_TRUE(controls.get("playerlooking"));

    auto stream = std::make_unique<std::stringstream>();
    {
        ESM::ESMWriter writer;
        writer.setFormatVersion(ESM::CurrentSaveGameFormatVersion);
        writer.save(*stream);
        Loading::Listener progress;
        controls.write(writer, progress);
    }

    ESM::ESMReader reader;
    reader.open(std::move(stream), "independent-player-control-switches");
    ASSERT_TRUE(reader.hasMoreRecs());
    ASSERT_EQ(reader.getRecName().toInt(), ESM::REC_INPU);
    reader.getRecHeader();

    MWInput::ControlSwitch restored;
    restored.readRecord(reader, ESM::REC_INPU);
    EXPECT_FALSE(restored.get("playermovement"));
    EXPECT_FALSE(restored.get("playerinterface"));
    EXPECT_FALSE(restored.get("playersneaking"));
    EXPECT_FALSE(restored.get("playerrollover"));
    EXPECT_TRUE(restored.get("playercontrols"));
    EXPECT_TRUE(restored.get("playerfighting"));
    EXPECT_TRUE(restored.get("playerviewswitch"));
    EXPECT_TRUE(restored.get("playerlooking"));
}
