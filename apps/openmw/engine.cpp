#include "engine.hpp"

#include <cerrno>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <osgDB/ReaderWriter>
#include <osgDB/Registry>
#include <osgViewer/ViewerEventHandlers>

#include <yaml-cpp/yaml.h>

#include <SDL.h>

#include <components/debug/debuglog.hpp>
#include <components/debug/gldebug.hpp>

#include <components/misc/rng.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/misc/strings/format.hpp>

#include <components/vfs/manager.hpp>
#include <components/vfs/registerarchives.hpp>

#include <components/sdlutil/imagetosurface.hpp>
#include <components/sdlutil/sdlgraphicswindow.hpp>

#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/resource/stats.hpp>

#include <components/compiler/extensions0.hpp>

#include <components/stereo/stereomanager.hpp>

#include <components/sceneutil/glextensions.hpp>
#include <components/sceneutil/workqueue.hpp>

#include <components/files/configurationmanager.hpp>

#include <components/version/version.hpp>

#include <components/l10n/manager.hpp>

#include <components/loadinglistener/asynclistener.hpp>
#include <components/loadinglistener/loadinglistener.hpp>

#include <components/misc/frameratelimiter.hpp>

#include <components/sceneutil/color.hpp>
#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/screencapture.hpp>
#include <components/sceneutil/unrefqueue.hpp>
#include <components/sceneutil/util.hpp>

#include <components/settings/shadermanager.hpp>
#include <components/settings/values.hpp>

#include "mwinput/inputmanagerimp.hpp"

#include "mwgui/windowmanagerimp.hpp"

#include "mwlua/luamanagerimp.hpp"
#include "mwlua/worker.hpp"

#include "mwscript/interpretercontext.hpp"
#include "mwscript/scriptmanagerimp.hpp"

#include "mwsound/constants.hpp"
#include "mwsound/soundmanagerimp.hpp"

#include "mwworld/class.hpp"
#include "mwworld/action.hpp"
#include "mwworld/cellstore.hpp"
#include "mwworld/datetimemanager.hpp"
#include "mwworld/esm4questruntime.hpp"
#include "mwworld/scene.hpp"
#include "mwworld/worldimp.hpp"

#include "mwrender/vismask.hpp"

#include "mwclass/classes.hpp"

#include "mwdialogue/dialoguemanagerimp.hpp"
#include "mwdialogue/journalimp.hpp"
#include "mwdialogue/scripttest.hpp"

#include "mwmechanics/mechanicsmanagerimp.hpp"

#include "mwstate/statemanagerimp.hpp"

#include "profile.hpp"

namespace
{
    void checkSDLError(int ret)
    {
        if (ret != 0)
            Log(Debug::Error) << "SDL error: " << SDL_GetError();
    }

    void initStatsHandler(Resource::Profiler& profiler)
    {
        const osg::Vec4f textColor(1.f, 1.f, 1.f, 1.f);
        const osg::Vec4f barColor(1.f, 1.f, 1.f, 1.f);
        const float multiplier = 1000;
        const bool average = true;
        const bool averageInInverseSpace = false;
        const float maxValue = 10000;

        OMW::forEachUserStatsValue([&](const OMW::UserStats& v) {
            profiler.addUserStatsLine(v.mLabel, textColor, barColor, v.mTaken, multiplier, average,
                averageInInverseSpace, v.mBegin, v.mEnd, maxValue);
        });
        // the forEachUserStatsValue loop is "run" at compile time, hence the settings manager is not available.
        // Unconditionnally add the async physics stats, and then remove it at runtime if necessary
        if (Settings::physics().mAsyncNumThreads == 0)
            profiler.removeUserStatsLine(" -Async");
    }

    struct ScreenCaptureMessageBox
    {
        void operator()(std::string filePath) const
        {
            if (filePath.empty())
            {
                MWBase::Environment::get().getWindowManager()->scheduleMessageBox(
                    "#{OMWEngine:ScreenshotFailed}", MWGui::ShowInDialogueMode_Never);

                return;
            }

            auto l10n = MWBase::Environment::get().getL10nManager()->getContext("OMWEngine");
            std::string message = l10n->formatMessage("ScreenshotMade", { "file" }, { L10n::toUnicode(filePath) });

            MWBase::Environment::get().getWindowManager()->scheduleMessageBox(
                std::move(message), MWGui::ShowInDialogueMode_Never);
        }
    };

    struct IgnoreString
    {
        void operator()(std::string) const {}
    };

    // Opt-in, declarative route runner for unattended compatibility evidence.
    // It deliberately uses only engine World APIs: it never synthesizes a
    // desktop event, changes focus, or knows anything about a particular
    // campaign beyond the route manifest supplied by the caller.
    struct CompatibilityRouteStep
    {
        std::string mId;
        std::string mOperation;
        std::string mQuest;
        std::optional<ESM::FormId> mReference;
        std::optional<ESM::FormId> mCell;
        osg::Vec3f mOffset{};
        std::uint8_t mStage = 0;
        bool mRequireStageDone = true;
        std::int32_t mObjective = 0;
        bool mObjectiveDisplayed = true;
        double mDurationSeconds = 0.0;
        double mTimeoutSeconds = 60.0;
    };

    std::optional<ESM::FormId> parseCompatibilityRouteFormId(std::string_view value)
    {
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
            value.remove_prefix(1);
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
            value.remove_suffix(1);
        constexpr std::string_view formPrefix = "FormId:";
        if (value.size() >= formPrefix.size()
            && Misc::StringUtils::ciEqual(value.substr(0, formPrefix.size()), formPrefix))
        {
            value.remove_prefix(formPrefix.size());
        }
        if (value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X'))
            value.remove_prefix(2);
        if (value.empty())
            return std::nullopt;

        std::uint32_t raw = 0;
        const char* const begin = value.data();
        const char* const end = begin + value.size();
        const auto parsed = std::from_chars(begin, end, raw, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != end || raw == 0)
            return std::nullopt;
        return ESM::FormId::fromUint32(raw);
    }

    void writeCompatibilityRouteJsonString(std::ostream& stream, std::string_view value)
    {
        stream << '"';
        for (const unsigned char ch : value)
        {
            switch (ch)
            {
                case '"': stream << "\\\""; break;
                case '\\': stream << "\\\\"; break;
                case '\b': stream << "\\b"; break;
                case '\f': stream << "\\f"; break;
                case '\n': stream << "\\n"; break;
                case '\r': stream << "\\r"; break;
                case '\t': stream << "\\t"; break;
                default:
                    if (ch < 0x20)
                        stream << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                               << static_cast<unsigned int>(ch) << std::dec << std::setfill(' ');
                    else
                        stream << static_cast<char>(ch);
                    break;
            }
        }
        stream << '"';
    }

    std::string compatibilityRouteStepStatus(bool completed, bool failed, bool running)
    {
        if (failed)
            return "failed";
        if (completed)
            return "passed";
        return running ? "running" : "pending";
    }

    class CompatibilityRouteDriver
    {
    public:
        static CompatibilityRouteDriver fromEnvironment()
        {
            CompatibilityRouteDriver result;
            const char* const routePath = std::getenv("OPENMW_COMPAT_ROUTE_PATH");
            if (routePath == nullptr || *routePath == '\0')
                return result;

            result.mEnabled = true;
            result.mRoutePath = std::filesystem::path(routePath);
            if (const char* const reportPath = std::getenv("OPENMW_COMPAT_ROUTE_REPORT_PATH"))
                result.mReportPath = std::filesystem::path(reportPath);
            result.mExitAfterFinish = std::getenv("OPENMW_COMPAT_ROUTE_EXIT_AFTER_WRITE") != nullptr;

            try
            {
                const YAML::Node root = YAML::LoadFile(result.mRoutePath.string());
                if (!root.IsMap())
                    throw std::runtime_error("route root must be a mapping");
                const std::string schema = root["schema"] ? root["schema"].as<std::string>() : "";
                if (schema != "opennv-authored-route/v1")
                    throw std::runtime_error("unsupported schema '" + schema + "'");
                result.mRouteId = root["id"] ? root["id"].as<std::string>() : result.mRoutePath.stem().string();
                const YAML::Node steps = root["steps"];
                if (!steps || !steps.IsSequence() || steps.size() == 0)
                    throw std::runtime_error("route must declare a non-empty steps sequence");

                std::size_t index = 0;
                for (const YAML::Node& node : steps)
                {
                    if (!node.IsMap())
                        throw std::runtime_error("step " + std::to_string(index) + " must be a mapping");
                    CompatibilityRouteStep step;
                    step.mId = node["id"] ? node["id"].as<std::string>() : "step-" + std::to_string(index);
                    step.mOperation = node["operation"] ? node["operation"].as<std::string>() : "";
                    if (step.mOperation.empty())
                        throw std::runtime_error("step '" + step.mId + "' is missing operation");
                    if (node["timeout_seconds"])
                        step.mTimeoutSeconds = node["timeout_seconds"].as<double>();
                    if (!std::isfinite(step.mTimeoutSeconds) || step.mTimeoutSeconds <= 0.0)
                        throw std::runtime_error("step '" + step.mId + "' has an invalid timeout_seconds");

                    if (step.mOperation == "wait-quest-stage")
                    {
                        step.mQuest = node["quest"] ? node["quest"].as<std::string>() : "";
                        const int stage = node["stage"] ? node["stage"].as<int>() : -1;
                        if (step.mQuest.empty() || stage < 0 || stage > 255)
                            throw std::runtime_error("step '" + step.mId + "' needs quest and stage in 0..255");
                        step.mStage = static_cast<std::uint8_t>(stage);
                        if (node["stage_done"])
                            step.mRequireStageDone = node["stage_done"].as<bool>();
                    }
                    else if (step.mOperation == "wait-quest-objective")
                    {
                        step.mQuest = node["quest"] ? node["quest"].as<std::string>() : "";
                        const int objective = node["objective"] ? node["objective"].as<int>() : -1;
                        if (step.mQuest.empty() || objective < 0)
                            throw std::runtime_error("step '" + step.mId + "' needs quest and a non-negative objective");
                        step.mObjective = objective;
                        if (node["displayed"])
                            step.mObjectiveDisplayed = node["displayed"].as<bool>();
                    }
                    else if (step.mOperation == "move-to-reference" || step.mOperation == "activate-reference"
                        || step.mOperation == "wait-reference-active")
                    {
                        const std::string reference = node["reference"] ? node["reference"].as<std::string>() : "";
                        step.mReference = parseCompatibilityRouteFormId(reference);
                        if (!step.mReference)
                            throw std::runtime_error("step '" + step.mId + "' needs a hexadecimal reference FormId");
                        if (node["offset"])
                        {
                            const YAML::Node offset = node["offset"];
                            if (!offset.IsSequence() || offset.size() != 3)
                                throw std::runtime_error("step '" + step.mId + "' offset must contain three values");
                            step.mOffset.set(offset[0].as<float>(), offset[1].as<float>(), offset[2].as<float>());
                        }
                    }
                    else if (step.mOperation == "wait-seconds")
                    {
                        step.mDurationSeconds = node["duration_seconds"] ? node["duration_seconds"].as<double>() : 0.0;
                        if (!std::isfinite(step.mDurationSeconds) || step.mDurationSeconds < 0.0)
                            throw std::runtime_error("step '" + step.mId + "' has an invalid duration_seconds");
                    }
                    else if (step.mOperation == "wait-player-exterior")
                    {
                    }
                    else if (step.mOperation == "wait-player-cell")
                    {
                        const std::string cell = node["cell"] ? node["cell"].as<std::string>() : "";
                        step.mCell = parseCompatibilityRouteFormId(cell);
                        if (!step.mCell)
                            throw std::runtime_error("step '" + step.mId + "' needs a hexadecimal cell FormId");
                    }
                    else
                        throw std::runtime_error("step '" + step.mId + "' has unsupported operation '" + step.mOperation
                            + "'");

                    result.mSteps.emplace_back(std::move(step));
                    ++index;
                }
                result.mStates.resize(result.mSteps.size());
            }
            catch (const std::exception& e)
            {
                result.mStates.resize(result.mSteps.size());
                result.mLoadError = e.what();
            }
            return result;
        }

        [[nodiscard]] bool enabled() const { return mEnabled; }
        [[nodiscard]] bool shouldQuit() const { return mFinished && mExitAfterFinish; }

        void update(MWWorld::World& world, unsigned frameNumber)
        {
            if (!mEnabled || mFinished)
                return;
            if (!mLoadError.empty())
            {
                fail(frameNumber, "route-load-failed: " + mLoadError);
                return;
            }
            if (mCurrentStep >= mSteps.size())
            {
                finish(frameNumber, true, "all declared steps completed");
                return;
            }

            CompatibilityRouteStep& step = mSteps[mCurrentStep];
            StepState& state = mStates[mCurrentStep];
            if (!state.mStarted)
            {
                state.mStarted = true;
                state.mStartFrame = frameNumber;
                state.mStartedAt = std::chrono::steady_clock::now();
                Log(Debug::Info) << "OpenNV compatibility route: id='" << mRouteId << "' step='" << step.mId
                                 << "' operation=" << step.mOperation << " frame=" << frameNumber;
            }

            try
            {
                bool completed = false;
                if (step.mOperation == "wait-quest-stage")
                {
                    const MWWorld::ESM4QuestState* quest = world.getESM4QuestRuntime().search(step.mQuest);
                    if (quest != nullptr)
                    {
                        const auto done = quest->mStageDone.find(step.mStage);
                        completed = step.mRequireStageDone ? (done != quest->mStageDone.end() && done->second)
                                                          : quest->mCurrentStage >= step.mStage;
                        state.mDetail = "quest=" + step.mQuest + " currentStage="
                            + std::to_string(quest->mCurrentStage) + " targetStage=" + std::to_string(step.mStage);
                    }
                    else
                        state.mDetail = "quest=" + step.mQuest + " unavailable";
                }
                else if (step.mOperation == "wait-quest-objective")
                {
                    const MWWorld::ESM4QuestState* quest = world.getESM4QuestRuntime().search(step.mQuest);
                    if (quest != nullptr)
                    {
                        const auto objective = quest->mObjectiveStatus.find(step.mObjective);
                        const std::uint8_t status = objective != quest->mObjectiveStatus.end() ? objective->second : 0;
                        const std::uint8_t flag = step.mObjectiveDisplayed
                            ? MWWorld::ESM4QuestState::Objective_Displayed
                            : MWWorld::ESM4QuestState::Objective_Completed;
                        completed = (status & flag) != 0;
                        state.mDetail = "quest=" + step.mQuest + " objective=" + std::to_string(step.mObjective)
                            + " status=" + std::to_string(status) + " target="
                            + (step.mObjectiveDisplayed ? "displayed" : "completed");
                    }
                    else
                        state.mDetail = "quest=" + step.mQuest + " unavailable";
                }
                else if (step.mOperation == "wait-seconds")
                {
                    completed = std::chrono::duration<double>(std::chrono::steady_clock::now() - state.mStartedAt).count()
                        >= step.mDurationSeconds;
                    state.mDetail = "durationSeconds=" + std::to_string(step.mDurationSeconds);
                }
                else if (step.mOperation == "wait-player-exterior")
                {
                    const MWWorld::Ptr player = world.getPlayerPtr();
                    completed = !player.isEmpty() && player.getCell() != nullptr && player.getCell()->isExterior();
                    state.mDetail = completed ? "player is exterior" : "player is not exterior";
                }
                else if (step.mOperation == "wait-player-cell")
                {
                    const MWWorld::Ptr player = world.getPlayerPtr();
                    completed = !player.isEmpty() && player.getCell() != nullptr && player.getCell()->getCell() != nullptr
                        && player.getCell()->getCell()->getId() == ESM::RefId(*step.mCell);
                    state.mDetail = completed ? "player reached declared cell" : "player has not reached declared cell";
                }
                else
                {
                    const MWWorld::Ptr reference = findActiveReference(world, *step.mReference);
                    if (step.mOperation == "wait-reference-active")
                    {
                        completed = !reference.isEmpty();
                        state.mDetail = completed ? "declared reference is active" : "declared reference is not active";
                    }
                    else if (reference.isEmpty())
                        state.mDetail = "declared reference is not active";
                    else if (step.mOperation == "move-to-reference")
                    {
                        const MWWorld::Ptr player = world.getPlayerPtr();
                        if (player.isEmpty() || reference.getCell() == nullptr)
                            state.mDetail = "player or target cell unavailable";
                        else
                        {
                            const osg::Vec3f target = reference.getRefData().getPosition().asVec3() + step.mOffset;
                            world.moveObject(player, reference.getCell(), target, true, false);
                            // A placed reference origin is often inside its collision geometry
                            // (and trigger origins are conventionally at the centre of their
                            // volume).  A route must arrive at its declared offset as a player
                            // would, on a collision surface, rather than leave the camera
                            // embedded in the reference or below the interior.
                            world.adjustPosition(player, true);
                            const ESM::Position& landed = player.getRefData().getPosition();
                            completed = true;
                            state.mDetail = "moved player near "
                                + reference.getCellRef().getRefNum().toString("FormId:") + " target=("
                                + std::to_string(target.x()) + "," + std::to_string(target.y()) + ","
                                + std::to_string(target.z()) + ") landed=(" + std::to_string(landed.pos[0]) + ","
                                + std::to_string(landed.pos[1]) + "," + std::to_string(landed.pos[2]) + ")";
                        }
                    }
                    else if (step.mOperation == "activate-reference")
                    {
                        const MWWorld::Ptr player = world.getPlayerPtr();
                        bool dispatchedAuthoredReferenceScript = false;
                        if (!player.isEmpty())
                        {
                            // Dispatch the placed reference's authored activation script before
                            // its normal activation action, matching the in-game player boundary.
                            dispatchedAuthoredReferenceScript
                                = world.getESM4QuestRuntime().onReferenceActivated(reference, player);
                        }
                        std::unique_ptr<MWWorld::Action> action
                            = player.isEmpty() ? nullptr : reference.getClass().activate(reference, player);
                        if (action == nullptr || action->isNullAction())
                        {
                            if (dispatchedAuthoredReferenceScript)
                            {
                                completed = true;
                                state.mDetail = "dispatched authored reference activation on "
                                    + reference.getCellRef().getRefNum().toString("FormId:");
                            }
                            else
                                state.mDetail = "declared reference produced no activation action";
                        }
                        else
                        {
                            action->execute(player);
                            completed = true;
                            state.mDetail = "executed authored activation on "
                                + reference.getCellRef().getRefNum().toString("FormId:");
                        }
                    }
                }

                if (completed)
                {
                    state.mCompleted = true;
                    state.mFinishFrame = frameNumber;
                    Log(Debug::Info) << "OpenNV compatibility route: id='" << mRouteId << "' step='" << step.mId
                                     << "' result=pass detail='" << state.mDetail << "' frame=" << frameNumber;
                    ++mCurrentStep;
                    if (mCurrentStep >= mSteps.size())
                        finish(frameNumber, true, "all declared steps completed");
                    return;
                }
            }
            catch (const std::exception& e)
            {
                fail(frameNumber, "step '" + step.mId + "' threw: " + e.what());
                return;
            }

            if (std::chrono::duration<double>(std::chrono::steady_clock::now() - state.mStartedAt).count()
                > step.mTimeoutSeconds)
            {
                fail(frameNumber, "step '" + step.mId + "' timed out: " + state.mDetail);
            }
        }

    private:
        struct StepState
        {
            bool mStarted = false;
            bool mCompleted = false;
            bool mFailed = false;
            unsigned mStartFrame = 0;
            unsigned mFinishFrame = 0;
            std::chrono::steady_clock::time_point mStartedAt{};
            std::string mDetail;
        };

        static MWWorld::Ptr findActiveReference(MWWorld::World& world, ESM::FormId id)
        {
            for (MWWorld::CellStore* cellstore : world.getWorldScene().getActiveCells())
            {
                if (cellstore == nullptr)
                    continue;
                MWWorld::Ptr result;
                cellstore->forEach([&](const MWWorld::Ptr& ptr) {
                    if (!ptr.isEmpty() && ptr.getCellRef().getRefNum() == id)
                    {
                        result = ptr;
                        return false;
                    }
                    return true;
                });
                if (!result.isEmpty())
                    return result;
            }
            return {};
        }

        void fail(unsigned frameNumber, std::string detail)
        {
            if (mCurrentStep < mStates.size())
            {
                mStates[mCurrentStep].mFailed = true;
                mStates[mCurrentStep].mFinishFrame = frameNumber;
                mStates[mCurrentStep].mDetail = detail;
            }
            finish(frameNumber, false, std::move(detail));
        }

        void finish(unsigned frameNumber, bool passed, std::string detail)
        {
            if (mFinished)
                return;
            mFinished = true;
            mPassed = passed;
            mResultDetail = std::move(detail);
            writeReport(frameNumber);
            Log(mPassed ? Debug::Info : Debug::Error) << "OpenNV compatibility route: id='" << mRouteId
                                                       << "' result=" << (mPassed ? "pass" : "fail")
                                                       << " detail='" << mResultDetail << "' frame=" << frameNumber;
        }

        void writeReport(unsigned frameNumber) const
        {
            if (mReportPath.empty())
                return;
            std::error_code error;
            const std::filesystem::path parent = mReportPath.parent_path();
            if (!parent.empty())
                std::filesystem::create_directories(parent, error);
            if (error)
            {
                Log(Debug::Error) << "OpenNV compatibility route: could not create report directory '"
                                  << parent.string() << "': " << error.message();
                return;
            }
            std::ofstream output(mReportPath, std::ios::out | std::ios::trunc);
            if (!output.is_open())
            {
                Log(Debug::Error) << "OpenNV compatibility route: could not write report '" << mReportPath.string()
                                  << "'";
                return;
            }
            output << "{\n  \"schema\": \"opennv-authored-route-report/v1\",\n  \"route\": ";
            writeCompatibilityRouteJsonString(output, mRouteId);
            output << ",\n  \"routePath\": ";
            writeCompatibilityRouteJsonString(output, mRoutePath.string());
            output << ",\n  \"status\": \"" << (mPassed ? "pass" : "fail") << "\",\n  \"frame\": "
                   << frameNumber << ",\n  \"resultDetail\": ";
            writeCompatibilityRouteJsonString(output, mResultDetail);
            output << ",\n  \"capture\": {\n    \"driver\": \"engine-internal declared route\","
                   << "\n    \"windowsAppControlUsed\": false,\n    \"foregroundActivationUsed\": false,"
                   << "\n    \"foregroundInputInjected\": false\n  },\n  \"steps\": [";
            for (std::size_t index = 0; index < mSteps.size(); ++index)
            {
                if (index != 0)
                    output << ',';
                const CompatibilityRouteStep& step = mSteps[index];
                const StepState& state = mStates[index];
                output << "\n    {\"id\":";
                writeCompatibilityRouteJsonString(output, step.mId);
                output << ",\"operation\":";
                writeCompatibilityRouteJsonString(output, step.mOperation);
                output << ",\"status\":";
                writeCompatibilityRouteJsonString(
                    output, compatibilityRouteStepStatus(state.mCompleted, state.mFailed, state.mStarted));
                output << ",\"startFrame\":" << state.mStartFrame << ",\"finishFrame\":" << state.mFinishFrame
                       << ",\"detail\":";
                writeCompatibilityRouteJsonString(output, state.mDetail);
                output << '}';
            }
            output << "\n  ]\n}\n";
        }

        bool mEnabled = false;
        bool mExitAfterFinish = false;
        bool mFinished = false;
        bool mPassed = false;
        std::filesystem::path mRoutePath;
        std::filesystem::path mReportPath;
        std::string mRouteId;
        std::string mLoadError;
        std::string mResultDetail;
        std::vector<CompatibilityRouteStep> mSteps;
        std::vector<StepState> mStates;
        std::size_t mCurrentStep = 0;
    };

    class IdentifyOpenGLOperation : public osg::GraphicsOperation
    {
    public:
        IdentifyOpenGLOperation()
            : GraphicsOperation("IdentifyOpenGLOperation", false)
        {
        }

        void operator()(osg::GraphicsContext* graphicsContext) override
        {
            Log(Debug::Info) << "OpenGL Vendor: " << glGetString(GL_VENDOR);
            Log(Debug::Info) << "OpenGL Renderer: " << glGetString(GL_RENDERER);
            Log(Debug::Info) << "OpenGL Version: " << glGetString(GL_VERSION);
            glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &mMaxTextureImageUnits);
        }

        int getMaxTextureImageUnits() const
        {
            if (mMaxTextureImageUnits == 0)
                throw std::logic_error("mMaxTextureImageUnits is not initialized");
            return mMaxTextureImageUnits;
        }

    private:
        int mMaxTextureImageUnits = 0;
    };

    void reportStats(unsigned frameNumber, osgViewer::Viewer& viewer, std::ostream& stream)
    {
        viewer.getViewerStats()->report(stream, frameNumber);
        osgViewer::Viewer::Cameras cameras;
        viewer.getCameras(cameras);
        for (osg::Camera* camera : cameras)
            camera->getStats()->report(stream, frameNumber);
    }
}

void OMW::Engine::executeLocalScripts()
{
    MWWorld::LocalScripts& localScripts = mWorld->getLocalScripts();

    localScripts.startIteration();
    std::pair<ESM::RefId, MWWorld::Ptr> script;
    while (localScripts.getNext(script))
    {
        MWScript::InterpreterContext interpreterContext(&script.second.getRefData().getLocals(), script.second);
        mScriptManager->run(script.first, interpreterContext);
    }
}

bool OMW::Engine::frame(unsigned frameNumber, float frametime)
{
    const osg::Timer_t frameStart = mViewer->getStartTick();
    const osg::Timer* const timer = osg::Timer::instance();
    osg::Stats* const stats = mViewer->getViewerStats();

    mEnvironment.setFrameDuration(frametime);

    try
    {
        // update input
        {
            ScopedProfile<UserStatsType::Input> profile(frameStart, frameNumber, *timer, *stats);
            mInputManager->update(frametime, false);
        }

        // When the window is minimized, pause the game. Currently this *has* to be here to work around a MyGUI bug.
        // If we are not currently rendering, then RenderItems will not be reused resulting in a memory leak upon
        // changing widget textures (fixed in MyGUI 3.3.2), and destroyed widgets will not be deleted (not fixed yet,
        // https://github.com/MyGUI/mygui/issues/21)
        {
            ScopedProfile<UserStatsType::Sound> profile(frameStart, frameNumber, *timer, *stats);

            if (!mWindowManager->isWindowVisible())
            {
                mSoundManager->pausePlayback();
                return false;
            }
            else
                mSoundManager->resumePlayback();

            // sound
            if (mUseSound)
                mSoundManager->update(frametime);
        }

        {
            ScopedProfile<UserStatsType::LuaSyncUpdate> profile(frameStart, frameNumber, *timer, *stats);
            // Should be called after input manager update and before any change to the game world.
            // It applies to the game world queued changes from the previous frame.
            mLuaManager->synchronizedUpdate();
        }

        // update game state
        {
            ScopedProfile<UserStatsType::State> profile(frameStart, frameNumber, *timer, *stats);
            mStateManager->update(frametime);
        }

        bool paused = mWorld->getTimeManager()->isPaused();

        {
            ScopedProfile<UserStatsType::Script> profile(frameStart, frameNumber, *timer, *stats);

            if (mStateManager->getState() != MWBase::StateManager::State_NoGame)
            {
                if (!mWindowManager->containsMode(MWGui::GM_MainMenu) || !paused)
                {
                    if (mWorld->getScriptsEnabled())
                    {
                        // local scripts
                        executeLocalScripts();

                        // global scripts
                        mScriptManager->getGlobalScripts().run();
                    }

                    mWorld->getWorldScene().markCellAsUnchanged();
                }

                if (!paused)
                {
                    double hours = (frametime * mWorld->getTimeManager()->getGameTimeScale()) / 3600.0;
                    mWorld->advanceTime(hours, true);
                    mWorld->rechargeItems(frametime, true);
                }
            }
        }

        // update mechanics
        {
            ScopedProfile<UserStatsType::Mechanics> profile(frameStart, frameNumber, *timer, *stats);

            if (mStateManager->getState() != MWBase::StateManager::State_NoGame)
            {
                mMechanicsManager->update(frametime, paused);
            }

            if (mStateManager->getState() == MWBase::StateManager::State_Running)
            {
                MWWorld::Ptr player = mWorld->getPlayerPtr();
                if (!paused && player.getClass().getCreatureStats(player).isDead())
                    mStateManager->endGame();
            }
        }

        // update physics
        {
            ScopedProfile<UserStatsType::Physics> profile(frameStart, frameNumber, *timer, *stats);

            if (mStateManager->getState() != MWBase::StateManager::State_NoGame)
            {
                mWorld->updatePhysics(frametime, paused, frameStart, frameNumber, *stats);
            }
        }

        // update world
        {
            ScopedProfile<UserStatsType::World> profile(frameStart, frameNumber, *timer, *stats);

            if (mStateManager->getState() != MWBase::StateManager::State_NoGame)
            {
                mWorld->update(frametime, paused);
            }
        }

        // update GUI
        {
            ScopedProfile<UserStatsType::Gui> profile(frameStart, frameNumber, *timer, *stats);
            mWindowManager->update(frametime);
        }
    }
    catch (const std::exception& e)
    {
        Log(Debug::Error) << "Error in frame: " << e.what();
    }

    const bool reportResource = stats->collectStats("resource");

    if (reportResource)
        stats->setAttribute(frameNumber, "UnrefQueue", static_cast<double>(mUnrefQueue->getSize()));

    mUnrefQueue->flush(*mWorkQueue);

    if (reportResource)
    {
        stats->setAttribute(frameNumber, "FrameNumber", frameNumber);

        mResourceSystem->reportStats(frameNumber, stats);

        stats->setAttribute(frameNumber, "WorkQueue", static_cast<double>(mWorkQueue->getNumItems()));
        stats->setAttribute(frameNumber, "WorkThread", static_cast<double>(mWorkQueue->getNumActiveThreads()));

        mMechanicsManager->reportStats(frameNumber, *stats);
        mWorld->reportStats(frameNumber, *stats);
        mLuaManager->reportStats(frameNumber, *stats);

        stats->setAttribute(frameNumber, "StringRefId Count", static_cast<double>(ESM::StringRefId::totalCount()));
    }

    mStereoManager->updateSettings(Settings::camera().mNearClip, Settings::camera().mViewingDistance);

    mViewer->eventTraversal();
    mViewer->updateTraversal();

    // update focus object for GUI
    {
        ScopedProfile<UserStatsType::Focus> profile(frameStart, frameNumber, *timer, *stats);
        mWorld->updateFocusObject();
    }

    // if there is a separate Lua thread, it starts the update now
    mLuaWorker->allowUpdate(frameStart, frameNumber, *stats);

    mViewer->renderingTraversals();

    mLuaWorker->finishUpdate(frameStart, frameNumber, *stats);

    return true;
}

OMW::Engine::Engine(Files::ConfigurationManager& configurationManager)
    : mWindow(nullptr)
    , mEncoding(ToUTF8::WINDOWS_1252)
    , mScreenCaptureOperation(nullptr)
    , mSelectDepthFormatOperation(new SceneUtil::SelectDepthFormatOperation())
    , mSelectColorFormatOperation(new SceneUtil::Color::SelectColorFormatOperation())
    , mStereoManager(nullptr)
    , mSkipMenu(false)
    , mUseSound(true)
    , mCompileAll(false)
    , mCompileAllDialogue(false)
    , mWarningsMode(1)
    , mScriptConsoleMode(false)
    , mActivationDistanceOverride(-1)
    , mGrab(true)
    , mExportFonts(false)
    , mRandomSeed(0)
    , mNewGame(false)
    , mCfgMgr(configurationManager)
    , mGlMaxTextureImageUnits(0)
{
#if SDL_VERSION_ATLEAST(2, 24, 0)
    SDL_SetHint(SDL_HINT_MAC_OPENGL_ASYNC_DISPATCH, "1");
#endif
    SDL_SetHint(SDL_HINT_ACCELEROMETER_AS_JOYSTICK, "0"); // We use only gamepads

    Uint32 flags
        = SDL_INIT_VIDEO | SDL_INIT_NOPARACHUTE | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_SENSOR;
    if (SDL_WasInit(flags) == 0)
    {
        SDL_SetMainReady();
        if (SDL_Init(flags) != 0)
        {
            throw std::runtime_error("Could not initialize SDL! " + std::string(SDL_GetError()));
        }
    }
}

OMW::Engine::~Engine()
{
    if (mScreenCaptureOperation != nullptr)
    {
        mScreenCaptureOperation->stop();
        mScreenCaptureOperation = nullptr;
    }
    mScreenCaptureHandler = nullptr;

    mMechanicsManager = nullptr;
    mDialogueManager = nullptr;
    mJournal = nullptr;
    mWindowManager = nullptr;
    mScriptManager = nullptr;
    mWorld = nullptr;
    mStereoManager = nullptr;
    mSoundManager = nullptr;
    mInputManager = nullptr;
    mStateManager = nullptr;
    mLuaWorker = nullptr;
    mLuaManager = nullptr;
    mL10nManager = nullptr;

    mScriptContext = nullptr;

    mUnrefQueue = nullptr;
    mWorkQueue = nullptr;

    mViewer = nullptr;

    mResourceSystem.reset();

    mEncoder = nullptr;

    if (mWindow)
    {
        SDL_DestroyWindow(mWindow);
        mWindow = nullptr;
    }

    SDL_Quit();

    Log(Debug::Info) << "Quitting peacefully.";
}

// Set data dir

void OMW::Engine::setDataDirs(const Files::PathContainer& dataDirs)
{
    mDataDirs = dataDirs;
    mDataDirs.insert(mDataDirs.begin(), mResDir / "vfs");
    mFileCollections = Files::Collections(mDataDirs);
}

// Add BSA archive
void OMW::Engine::addArchive(const std::string& archive)
{
    mArchives.push_back(archive);
}

// Set resource dir
void OMW::Engine::setResourceDir(const std::filesystem::path& parResDir)
{
    mResDir = parResDir;
    if (!Version::checkResourcesVersion(mResDir))
        Log(Debug::Error) << "Resources dir " << mResDir
                          << " doesn't match OpenMW binary, the game may work incorrectly.";
}

// Set start cell name
void OMW::Engine::setCell(const std::string& cellName)
{
    mCellName = cellName;
}

void OMW::Engine::addContentFile(const std::string& file)
{
    mContentFiles.push_back(file);
}

void OMW::Engine::addGroundcoverFile(const std::string& file)
{
    mGroundcoverFiles.emplace_back(file);
}

void OMW::Engine::setSkipMenu(bool skipMenu, bool newGame)
{
    mSkipMenu = skipMenu;
    mNewGame = newGame;
}

void OMW::Engine::createWindow()
{
    const int screen = Settings::video().mScreen;
    const int width = Settings::video().mResolutionX;
    const int height = Settings::video().mResolutionY;
    const Settings::WindowMode windowMode = Settings::video().mWindowMode;
    const bool windowBorder = Settings::video().mWindowBorder;
    const SDLUtil::VSyncMode vsync = Settings::video().mVsyncMode;
    unsigned antialiasing = static_cast<unsigned>(Settings::video().mAntialiasing);

    int posX = SDL_WINDOWPOS_CENTERED_DISPLAY(screen);
    int posY = SDL_WINDOWPOS_CENTERED_DISPLAY(screen);

    if (windowMode == Settings::WindowMode::Fullscreen || windowMode == Settings::WindowMode::WindowedFullscreen)
    {
        posX = SDL_WINDOWPOS_UNDEFINED_DISPLAY(screen);
        posY = SDL_WINDOWPOS_UNDEFINED_DISPLAY(screen);
    }

    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (windowMode == Settings::WindowMode::Fullscreen)
        flags |= SDL_WINDOW_FULLSCREEN;
    else if (windowMode == Settings::WindowMode::WindowedFullscreen)
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

    // Allows for Windows snapping features to properly work in borderless window
    SDL_SetHint("SDL_BORDERLESS_WINDOWED_STYLE", "1");
    SDL_SetHint("SDL_BORDERLESS_RESIZABLE_STYLE", "1");

    if (!windowBorder)
        flags |= SDL_WINDOW_BORDERLESS;

    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, Settings::video().mMinimizeOnFocusLoss ? "1" : "0");

    checkSDLError(SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8));
    checkSDLError(SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8));
    checkSDLError(SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8));
    checkSDLError(SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0));
    checkSDLError(SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24));
    if (Debug::shouldDebugOpenGL())
        checkSDLError(SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG));

    if (antialiasing > 0)
    {
        checkSDLError(SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1));
        checkSDLError(SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, antialiasing));
    }

    osg::ref_ptr<SDLUtil::GraphicsWindowSDL2> graphicsWindow;
    while (!graphicsWindow || !graphicsWindow->valid())
    {
        while (!mWindow)
        {
            mWindow = SDL_CreateWindow("OpenMW", posX, posY, width, height, flags);
            if (!mWindow)
            {
                // Try with a lower AA
                if (antialiasing > 0)
                {
                    Log(Debug::Warning) << "Warning: " << antialiasing << "x antialiasing not supported, trying "
                                        << antialiasing / 2;
                    antialiasing /= 2;
                    Settings::video().mAntialiasing.set(antialiasing);
                    checkSDLError(SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, antialiasing));
                    continue;
                }
                else
                {
                    std::stringstream error;
                    error << "Failed to create SDL window: " << SDL_GetError();
                    throw std::runtime_error(error.str());
                }
            }
        }

        // Since we use physical resolution internally, we have to create the window with scaled resolution,
        // but we can't get the scale before the window exists, so instead we have to resize aftewards.
        int w, h;
        SDL_GetWindowSize(mWindow, &w, &h);
        int dw, dh;
        SDL_GL_GetDrawableSize(mWindow, &dw, &dh);
        if (dw != w || dh != h)
        {
            SDL_SetWindowSize(mWindow, width / (dw / w), height / (dh / h));
        }

        setWindowIcon();

        osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
        SDL_GetWindowPosition(mWindow, &traits->x, &traits->y);
        SDL_GL_GetDrawableSize(mWindow, &traits->width, &traits->height);
        traits->windowName = SDL_GetWindowTitle(mWindow);
        traits->windowDecoration = !(SDL_GetWindowFlags(mWindow) & SDL_WINDOW_BORDERLESS);
        traits->screenNum = SDL_GetWindowDisplayIndex(mWindow);
        traits->vsync = 0;
        traits->inheritedWindowData = new SDLUtil::GraphicsWindowSDL2::WindowData(mWindow);

        graphicsWindow = new SDLUtil::GraphicsWindowSDL2(traits, vsync);
        if (!graphicsWindow->valid())
            throw std::runtime_error("Failed to create GraphicsContext");

        if (traits->samples < antialiasing)
        {
            Log(Debug::Warning) << "Warning: Framebuffer MSAA level is only " << traits->samples << "x instead of "
                                << antialiasing << "x. Trying " << antialiasing / 2 << "x instead.";
            graphicsWindow->closeImplementation();
            SDL_DestroyWindow(mWindow);
            mWindow = nullptr;
            antialiasing /= 2;
            Settings::video().mAntialiasing.set(antialiasing);
            checkSDLError(SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, antialiasing));
            continue;
        }

        if (traits->red < 8)
            Log(Debug::Warning) << "Warning: Framebuffer only has a " << traits->red << " bit red channel.";
        if (traits->green < 8)
            Log(Debug::Warning) << "Warning: Framebuffer only has a " << traits->green << " bit green channel.";
        if (traits->blue < 8)
            Log(Debug::Warning) << "Warning: Framebuffer only has a " << traits->blue << " bit blue channel.";
        if (traits->depth < 24)
            Log(Debug::Warning) << "Warning: Framebuffer only has " << traits->depth << " bits of depth precision.";

        traits->alpha = 0; // set to 0 to stop ScreenCaptureHandler reading the alpha channel
    }

    osg::ref_ptr<osg::Camera> camera = mViewer->getCamera();
    camera->setGraphicsContext(graphicsWindow);
    camera->setViewport(0, 0, graphicsWindow->getTraits()->width, graphicsWindow->getTraits()->height);

    osg::ref_ptr<SceneUtil::OperationSequence> realizeOperations = new SceneUtil::OperationSequence(false);
    mViewer->setRealizeOperation(realizeOperations);
    osg::ref_ptr<IdentifyOpenGLOperation> identifyOp = new IdentifyOpenGLOperation();
    realizeOperations->add(identifyOp);
    realizeOperations->add(new SceneUtil::GetGLExtensionsOperation());

    if (Debug::shouldDebugOpenGL())
        realizeOperations->add(new Debug::EnableGLDebugOperation());

    realizeOperations->add(mSelectDepthFormatOperation);
    realizeOperations->add(mSelectColorFormatOperation);

    if (Stereo::getStereo())
    {
        Stereo::Settings settings;

        settings.mMultiview = Settings::stereo().mMultiview;
        settings.mAllowDisplayListsForMultiview = Settings::stereo().mAllowDisplayListsForMultiview;
        settings.mSharedShadowMaps = Settings::stereo().mSharedShadowMaps;

        if (Settings::stereo().mUseCustomView)
        {
            const osg::Vec3 leftEyeOffset(Settings::stereoView().mLeftEyeOffsetX,
                Settings::stereoView().mLeftEyeOffsetY, Settings::stereoView().mLeftEyeOffsetZ);

            const osg::Quat leftEyeOrientation(Settings::stereoView().mLeftEyeOrientationX,
                Settings::stereoView().mLeftEyeOrientationY, Settings::stereoView().mLeftEyeOrientationZ,
                Settings::stereoView().mLeftEyeOrientationW);

            const osg::Vec3 rightEyeOffset(Settings::stereoView().mRightEyeOffsetX,
                Settings::stereoView().mRightEyeOffsetY, Settings::stereoView().mRightEyeOffsetZ);

            const osg::Quat rightEyeOrientation(Settings::stereoView().mRightEyeOrientationX,
                Settings::stereoView().mRightEyeOrientationY, Settings::stereoView().mRightEyeOrientationZ,
                Settings::stereoView().mRightEyeOrientationW);

            settings.mCustomView = Stereo::CustomView{
                .mLeft = Stereo::View{
                    .pose = Stereo::Pose{
                        .position = leftEyeOffset,
                        .orientation = leftEyeOrientation,
                    },
                    .fov = Stereo::FieldOfView{
                        .angleLeft = Settings::stereoView().mLeftEyeFovLeft,
                        .angleRight = Settings::stereoView().mLeftEyeFovRight,
                        .angleUp = Settings::stereoView().mLeftEyeFovUp,
                        .angleDown = Settings::stereoView().mLeftEyeFovDown,
                    },
                },
                .mRight = Stereo::View{
                    .pose = Stereo::Pose{
                        .position = rightEyeOffset,
                        .orientation = rightEyeOrientation,
                    },
                    .fov = Stereo::FieldOfView{
                        .angleLeft = Settings::stereoView().mRightEyeFovLeft,
                        .angleRight = Settings::stereoView().mRightEyeFovRight,
                        .angleUp = Settings::stereoView().mRightEyeFovUp,
                        .angleDown = Settings::stereoView().mRightEyeFovDown,
                    },
                },
            };
        }

        if (Settings::stereo().mUseCustomEyeResolution)
            settings.mEyeResolution
                = osg::Vec2i(Settings::stereoView().mEyeResolutionX, Settings::stereoView().mEyeResolutionY);

        realizeOperations->add(new Stereo::InitializeStereoOperation(settings));
    }

    mViewer->realize();
    mGlMaxTextureImageUnits = identifyOp->getMaxTextureImageUnits();

    mViewer->getEventQueue()->getCurrentEventState()->setWindowRectangle(
        0, 0, graphicsWindow->getTraits()->width, graphicsWindow->getTraits()->height);
}

void OMW::Engine::setWindowIcon()
{
    std::ifstream windowIconStream;
    const auto windowIcon = mResDir / "openmw.png";
    windowIconStream.open(windowIcon, std::ios_base::in | std::ios_base::binary);
    if (windowIconStream.fail())
        Log(Debug::Error) << "Error: Failed to open " << windowIcon;
    osgDB::ReaderWriter* reader = osgDB::Registry::instance()->getReaderWriterForExtension("png");
    if (!reader)
    {
        Log(Debug::Error) << "Error: Failed to read window icon, no png readerwriter found";
        return;
    }
    osgDB::ReaderWriter::ReadResult result = reader->readImage(windowIconStream);
    if (!result.success())
        Log(Debug::Error) << "Error: Failed to read " << windowIcon << ": " << result.message() << " code "
                          << result.status();
    else
    {
        osg::ref_ptr<osg::Image> image = result.getImage();
        auto surface = SDLUtil::imageToSurface(image, true);
        SDL_SetWindowIcon(mWindow, surface.get());
    }
}

void OMW::Engine::prepareEngine()
{
    mStateManager = std::make_unique<MWState::StateManager>(mCfgMgr.getUserDataPath() / "saves", mContentFiles);
    mEnvironment.setStateManager(*mStateManager);

    const bool stereoEnabled = Settings::stereo().mStereoEnabled || osg::DisplaySettings::instance().get()->getStereo();
    mStereoManager = std::make_unique<Stereo::Manager>(
        mViewer, stereoEnabled, Settings::camera().mNearClip, Settings::camera().mViewingDistance);

    osg::ref_ptr<osg::Group> rootNode(new osg::Group);
    mViewer->setSceneData(rootNode);

    createWindow();

    mVFS = std::make_unique<VFS::Manager>();

    VFS::registerArchives(mVFS.get(), mFileCollections, mArchives, true, &mEncoder.get()->getStatelessEncoder());

    mResourceSystem = std::make_unique<Resource::ResourceSystem>(
        mVFS.get(), Settings::cells().mCacheExpiryDelay, &mEncoder.get()->getStatelessEncoder());
    mResourceSystem->getSceneManager()->getShaderManager().setMaxTextureUnits(mGlMaxTextureImageUnits);
    mResourceSystem->getSceneManager()->setUnRefImageDataAfterApply(
        false); // keep to Off for now to allow better state sharing
    mResourceSystem->getSceneManager()->setFilterSettings(Settings::general().mTextureMagFilter,
        Settings::general().mTextureMinFilter, Settings::general().mTextureMipmap,
        static_cast<float>(Settings::general().mAnisotropy));
    mEnvironment.setResourceSystem(*mResourceSystem);

    mWorkQueue = new SceneUtil::WorkQueue(Settings::cells().mPreloadNumThreads);
    mUnrefQueue = std::make_unique<SceneUtil::UnrefQueue>();

    mScreenCaptureOperation = new SceneUtil::AsyncScreenCaptureOperation(mWorkQueue,
        new SceneUtil::WriteScreenshotToFileOperation(mCfgMgr.getScreenshotPath(),
            Settings::general().mScreenshotFormat,
            Settings::general().mNotifyOnSavedScreenshot ? std::function<void(std::string)>(ScreenCaptureMessageBox{})
                                                         : std::function<void(std::string)>(IgnoreString{})));

    mScreenCaptureHandler = new osgViewer::ScreenCaptureHandler(mScreenCaptureOperation);

    mViewer->addEventHandler(mScreenCaptureHandler);

    mL10nManager = std::make_unique<L10n::Manager>(mVFS.get());
    mL10nManager->setPreferredLocales(Settings::general().mPreferredLocales, Settings::general().mGmstOverridesL10n);
    mEnvironment.setL10nManager(*mL10nManager);

    mLuaManager = std::make_unique<MWLua::LuaManager>(mVFS.get(), mResDir / "lua_libs");
    mEnvironment.setLuaManager(*mLuaManager);

    // Create input and UI first to set up a bootstrapping environment for
    // showing a loading screen and keeping the window responsive while doing so

    const auto keybinderUser = mCfgMgr.getUserConfigPath() / "input_v3.xml";
    bool keybinderUserExists = std::filesystem::exists(keybinderUser);
    if (!keybinderUserExists)
    {
        const auto input2 = (mCfgMgr.getUserConfigPath() / "input_v2.xml");
        if (std::filesystem::exists(input2))
        {
            keybinderUserExists = std::filesystem::copy_file(input2, keybinderUser);
            Log(Debug::Info) << "Loading keybindings file: " << keybinderUser;
        }
    }
    else
        Log(Debug::Info) << "Loading keybindings file: " << keybinderUser;

    const auto userdefault = mCfgMgr.getUserConfigPath() / "gamecontrollerdb.txt";
    const auto localdefault = mCfgMgr.getLocalPath() / "gamecontrollerdb.txt";

    std::filesystem::path userGameControllerdb;
    if (std::filesystem::exists(userdefault))
        userGameControllerdb = userdefault;

    std::filesystem::path gameControllerdb;
    if (std::filesystem::exists(localdefault))
        gameControllerdb = localdefault;
    else if (!mCfgMgr.getGlobalPath().empty())
    {
        const auto globaldefault = mCfgMgr.getGlobalPath() / "gamecontrollerdb.txt";
        if (std::filesystem::exists(globaldefault))
            gameControllerdb = globaldefault;
    }
    // else if it doesn't exist, pass in an empty path

    // gui needs our shaders path before everything else
    mResourceSystem->getSceneManager()->setShaderPath(mResDir / "shaders");

    osg::GLExtensions& exts = SceneUtil::getGLExtensions();

#if OSG_VERSION_LESS_THAN(3, 6, 6)
    // hack fix for https://github.com/openscenegraph/OpenSceneGraph/issues/1028
    if (!osg::isGLExtensionSupported(exts.contextID, "NV_framebuffer_multisample_coverage"))
        exts.glRenderbufferStorageMultisampleCoverageNV = nullptr;
#endif

    osg::ref_ptr<osg::Group> guiRoot = new osg::Group;
    guiRoot->setName("GUI Root");
    guiRoot->setNodeMask(MWRender::Mask_GUI);
    mStereoManager->disableStereoForNode(guiRoot);
    rootNode->addChild(guiRoot);

    mWindowManager = std::make_unique<MWGui::WindowManager>(mWindow, mViewer, guiRoot, mResourceSystem.get(),
        mWorkQueue.get(), mCfgMgr.getLogPath(), mScriptConsoleMode, mTranslationDataStorage, mEncoding, mExportFonts,
        Version::getOpenmwVersionDescription(), mCfgMgr);
    mEnvironment.setWindowManager(*mWindowManager);

    mInputManager = std::make_unique<MWInput::InputManager>(mWindow, mViewer, mScreenCaptureHandler, keybinderUser,
        keybinderUserExists, userGameControllerdb, gameControllerdb, mGrab);
    mEnvironment.setInputManager(*mInputManager);

    // Create sound system
    mSoundManager = std::make_unique<MWSound::SoundManager>(mVFS.get(), mUseSound);
    mEnvironment.setSoundManager(*mSoundManager);

    // Create the world
    mWorld = std::make_unique<MWWorld::World>(
        mResourceSystem.get(), mActivationDistanceOverride, mCellName, mCfgMgr.getUserDataPath());
    mEnvironment.setWorld(*mWorld);
    mEnvironment.setWorldModel(mWorld->getWorldModel());
    mEnvironment.setESMStore(mWorld->getStore());

    const MWWorld::Store<ESM::GameSetting>* gmst = &mWorld->getStore().get<ESM::GameSetting>();
    mL10nManager->setGmstLoader([gmst, misses = std::set<std::string, Misc::StringUtils::CiComp>()](
                                    std::string_view gmstName) mutable -> const std::string* {
        const ESM::GameSetting* res = gmst->search(gmstName);
        if (res && res->mValue.getType() == ESM::VT_String)
            return &res->mValue.getString();
        if (misses.emplace(gmstName).second)
            Log(Debug::Error) << "GMST " << gmstName << " not found";
        return nullptr;
    });

    mWindowManager->setStore(mWorld->getStore());

    // Load translation data
    mTranslationDataStorage.setEncoder(mEncoder.get());
    for (auto& mContentFile : mContentFiles)
        mTranslationDataStorage.loadTranslationData(mFileCollections, mContentFile);

    Compiler::registerExtensions(mExtensions);

    // Create script system
    mScriptContext = std::make_unique<MWScript::CompilerContext>(MWScript::CompilerContext::Type_Full);
    mScriptContext->setExtensions(&mExtensions);

    mScriptManager = std::make_unique<MWScript::ScriptManager>(mWorld->getStore(), *mScriptContext, mWarningsMode);
    mEnvironment.setScriptManager(*mScriptManager);

    // Create game mechanics system
    mMechanicsManager = std::make_unique<MWMechanics::MechanicsManager>();
    mEnvironment.setMechanicsManager(*mMechanicsManager);

    // Create dialog system
    mJournal = std::make_unique<MWDialogue::Journal>();
    mEnvironment.setJournal(*mJournal);

    mDialogueManager = std::make_unique<MWDialogue::DialogueManager>(mExtensions, mTranslationDataStorage);
    mEnvironment.setDialogueManager(*mDialogueManager);

    mLuaManager->loadPermanentStorage(mCfgMgr.getUserConfigPath());
    mLuaManager->initPreLoad();

    Loading::Listener* listener = MWBase::Environment::get().getWindowManager()->getLoadingScreen();
    Loading::AsyncListener asyncListener(*listener);
    auto dataLoading = std::async(std::launch::async,
        [&] { mWorld->loadData(mFileCollections, mContentFiles, mGroundcoverFiles, mEncoder.get(), &asyncListener); });

    if (!mSkipMenu)
    {
        std::string_view logo = Fallback::Map::getString("Movies_Company_Logo");
        if (!logo.empty())
            mWindowManager->playVideo(logo, true);
    }

    listener->loadingOn();
    {
        using namespace std::chrono_literals;
        while (dataLoading.wait_for(50ms) != std::future_status::ready)
            asyncListener.update();
        dataLoading.get();
    }
    listener->loadingOff();

    mWorld->init(mMaxRecastLogLevel, mViewer, std::move(rootNode), mWorkQueue.get(), *mUnrefQueue);
    Log(Debug::Info) << "OpenNV startup: world initialized";
    mEnvironment.setWorldScene(mWorld->getWorldScene());
    mWorld->setupPlayer();
    Log(Debug::Info) << "OpenNV startup: player setup";
    mWorld->setRandomSeed(mRandomSeed);
    mWindowManager->initUI();
    Log(Debug::Info) << "OpenNV startup: UI initialized";
    mLuaManager->initPostLoad();
    Log(Debug::Info) << "OpenNV startup: Lua post-load initialized";

    // scripts
    if (mCompileAll)
    {
        std::pair<int, int> result = mScriptManager->compileAll();
        if (result.first)
            Log(Debug::Info) << "compiled " << result.second << " of " << result.first << " scripts ("
                             << 100 * static_cast<double>(result.second) / result.first << "%)";
    }
    if (mCompileAllDialogue)
    {
        std::pair<int, int> result = MWDialogue::ScriptTest::compileAll(&mExtensions, mWarningsMode);
        if (result.first)
            Log(Debug::Info) << "compiled " << result.second << " of " << result.first << " dialogue scripts ("
                             << 100 * static_cast<double>(result.second) / result.first << "%)";
    }

    // starts a separate lua thread if "lua num threads" > 0
    mLuaWorker = std::make_unique<MWLua::Worker>(*mLuaManager);
}

// Initialise and enter main loop.
void OMW::Engine::go()
{
    assert(!mContentFiles.empty());

    Log(Debug::Info) << "OSG version: " << osgGetVersion();
    SDL_version sdlVersion;
    SDL_GetVersion(&sdlVersion);
    Log(Debug::Info) << "SDL version: " << (int)sdlVersion.major << "." << (int)sdlVersion.minor << "."
                     << (int)sdlVersion.patch;

    Misc::Rng::init(mRandomSeed);

    Settings::ShaderManager::get().load(mCfgMgr.getUserConfigPath() / "shaders.yaml");

    MWClass::registerClasses();

    // Create encoder
    mEncoder = std::make_unique<ToUTF8::Utf8Encoder>(mEncoding);

    // Setup viewer
    mViewer = new osgViewer::Viewer;
    mViewer->setReleaseContextAtEndOfFrameHint(false);

    // Do not try to outsmart the OS thread scheduler (see bug #4785).
    mViewer->setUseConfigureAffinity(false);

    mEnvironment.setFrameRateLimit(Settings::video().mFramerateLimit);

    prepareEngine();

#ifdef _WIN32
    const auto* statsFile = _wgetenv(L"OPENMW_OSG_STATS_FILE");
#else
    const auto* statsFile = std::getenv("OPENMW_OSG_STATS_FILE");
#endif

    std::filesystem::path path;
    if (statsFile != nullptr)
        path = statsFile;

    std::ofstream stats;
    if (!path.empty())
    {
        stats.open(path, std::ios_base::out);
        if (stats.is_open())
            Log(Debug::Info) << "OSG stats will be written to: " << path;
        else
            Log(Debug::Warning) << "Failed to open file to write OSG stats \"" << path
                                << "\": " << std::generic_category().message(errno);
    }

    // Setup profiler
    osg::ref_ptr<Resource::Profiler> statsHandler = new Resource::Profiler(stats.is_open(), *mVFS);

    initStatsHandler(*statsHandler);

    mViewer->addEventHandler(statsHandler);

    osg::ref_ptr<Resource::StatsHandler> resourcesHandler = new Resource::StatsHandler(stats.is_open(), *mVFS);
    mViewer->addEventHandler(resourcesHandler);

    if (stats.is_open())
        Resource::collectStatistics(*mViewer);

    // Start the game
    if (!mSaveGameFile.empty())
    {
        mStateManager->loadGame(mSaveGameFile);
    }
    else if (!mSkipMenu)
    {
        // start in main menu
        mWindowManager->pushGuiMode(MWGui::GM_MainMenu);

        if (mVFS->exists(MWSound::titleMusic))
            mSoundManager->streamMusic(MWSound::titleMusic, MWSound::MusicType::Normal);
        else
            Log(Debug::Warning) << "Title music not found";

        std::string_view logo = Fallback::Map::getString("Movies_Morrowind_Logo");
        if (!logo.empty())
            mWindowManager->playVideo(logo, /*allowSkipping*/ true, /*overrideSounds*/ false);
    }
    else
    {
        mStateManager->newGame(!mNewGame);
    }

    if (!mStartupScript.empty() && mStateManager->getState() == MWState::StateManager::State_Running)
    {
        mWindowManager->executeInConsole(mStartupScript);
    }

    // Start the main rendering loop
    MWWorld::DateTimeManager& timeManager = *mWorld->getTimeManager();
    CompatibilityRouteDriver compatibilityRoute = CompatibilityRouteDriver::fromEnvironment();
    if (compatibilityRoute.enabled())
        Log(Debug::Info) << "OpenNV compatibility route: armed";
    Misc::FrameRateLimiter frameRateLimiter = Misc::makeFrameRateLimiter(mEnvironment.getFrameRateLimit());
    const std::chrono::steady_clock::duration maxSimulationInterval(std::chrono::milliseconds(200));
    while (!mViewer->done() && !mStateManager->hasQuitRequest())
    {
        const double dt = std::chrono::duration_cast<std::chrono::duration<double>>(
                              std::min(frameRateLimiter.getLastFrameDuration(), maxSimulationInterval))
                              .count()
            * timeManager.getSimulationTimeScale();

        mViewer->advance(timeManager.getRenderingSimulationTime());

        const unsigned frameNumber = mViewer->getFrameStamp()->getFrameNumber();

        if (!frame(frameNumber, static_cast<float>(dt)))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        timeManager.updateIsPaused();
        if (!timeManager.isPaused())
        {
            timeManager.setSimulationTime(timeManager.getSimulationTime() + dt);
            timeManager.setRenderingSimulationTime(timeManager.getRenderingSimulationTime() + dt);
        }

        if (compatibilityRoute.enabled())
        {
            compatibilityRoute.update(*mWorld, frameNumber);
            if (compatibilityRoute.shouldQuit())
                mStateManager->requestQuit();
        }

        if (stats)
        {
            // The delay is required because rendering happens in parallel to the main thread and stats from there is
            // available with delay.
            constexpr unsigned statsReportDelay = 3;
            if (frameNumber >= statsReportDelay)
            {
                // Viewer frame number can be different from frameNumber because of loading screens which render new
                // frames inside a simulation frame.
                const unsigned currentFrameNumber = mViewer->getFrameStamp()->getFrameNumber();
                for (unsigned i = frameNumber; i <= currentFrameNumber; ++i)
                    reportStats(i - statsReportDelay, *mViewer, stats);
            }
        }

        frameRateLimiter.limit();
    }

    mLuaWorker->join();

    // Save user settings
    Settings::Manager::saveUser(mCfgMgr.getUserConfigPath() / "settings.cfg");
    Settings::ShaderManager::get().save();
    mLuaManager->savePermanentStorage(mCfgMgr.getUserConfigPath());
}

void OMW::Engine::setCompileAll(bool all)
{
    mCompileAll = all;
}

void OMW::Engine::setCompileAllDialogue(bool all)
{
    mCompileAllDialogue = all;
}

void OMW::Engine::setSoundUsage(bool soundUsage)
{
    mUseSound = soundUsage;
}

void OMW::Engine::setEncoding(const ToUTF8::FromType& encoding)
{
    mEncoding = encoding;
}

void OMW::Engine::setScriptConsoleMode(bool enabled)
{
    mScriptConsoleMode = enabled;
}

void OMW::Engine::setStartupScript(const std::filesystem::path& path)
{
    mStartupScript = path;
}

void OMW::Engine::setActivationDistanceOverride(int distance)
{
    mActivationDistanceOverride = distance;
}

void OMW::Engine::setWarningsMode(int mode)
{
    mWarningsMode = mode;
}

void OMW::Engine::enableFontExport(bool exportFonts)
{
    mExportFonts = exportFonts;
}

void OMW::Engine::setSaveGameFile(const std::filesystem::path& savegame)
{
    mSaveGameFile = savegame;
}

void OMW::Engine::setRandomSeed(unsigned int seed)
{
    mRandomSeed = seed;
}
