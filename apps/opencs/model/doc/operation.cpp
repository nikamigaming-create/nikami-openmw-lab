#include "operation.hpp"

#include <algorithm>
#include <exception>
#include <vector>

#include <QCoreApplication>

#include <components/debug/debuglog.hpp>

#include <apps/opencs/model/doc/messages.hpp>

#include <components/debug/debuglog.hpp>

#include <apps/opencs/model/doc/messages.hpp>

#include "../world/universalid.hpp"

#include "stage.hpp"

namespace CSMDoc
{
    namespace
    {
        std::string_view operationToString(State value)
        {
            switch (value)
            {
                case State_Saving:
                    return "Saving";
                case State_Merging:
                    return "Merging";
                case State_Verifying:
                    return "Verifying";
                case State_Searching:
                    return "Searching";
                case State_Loading:
                    return "Loading";
                default:
                    break;
            }
            return "Unknown";
        }
    }
}

void CSMDoc::Operation::prepareStages()
{
    mCurrentStage = mStages.begin();
    mCurrentStep = 0;
    mCurrentStepTotal = 0;
    mTotalSteps = 0;
    mError = false;

    for (std::vector<std::pair<Stage*, int>>::iterator iter(mStages.begin()); iter != mStages.end(); ++iter)
    {
        iter->second = iter->first->setup();
        mTotalSteps += iter->second;
    }
}

<<<<<<< HEAD
CSMDoc::Operation::Operation(State type, bool finalAlways)
=======
CSMDoc::Operation::Operation(State type, bool ordered, bool finalAlways)
>>>>>>> origin/main
    : mType(type)
    , mStages(std::vector<std::pair<Stage*, int>>())
    , mCurrentStage(mStages.begin())
    , mCurrentStep(0)
    , mCurrentStepTotal(0)
    , mTotalSteps(0)
<<<<<<< HEAD
    , mFinalAlways(finalAlways)
    , mError(false)
    , mPrepared(false)
    , mDefaultSeverity(Message::Severity_Error)
{
=======
    , mOrdered(ordered)
    , mFinalAlways(finalAlways)
    , mError(false)
    , mConnected(false)
    , mPrepared(false)
    , mDefaultSeverity(Message::Severity_Error)
{
    mTimer = new QTimer(this);
>>>>>>> origin/main
}

CSMDoc::Operation::~Operation()
{
    for (std::vector<std::pair<Stage*, int>>::iterator iter(mStages.begin()); iter != mStages.end(); ++iter)
        delete iter->first;
}

void CSMDoc::Operation::run()
{
<<<<<<< HEAD
    mPrepared = false;
    mStart = std::chrono::steady_clock::now();
    QMetaObject::invokeMethod(this, &Operation::executeStage, Qt::QueuedConnection);
=======
    mTimer->stop();

    if (!mConnected)
    {
        connect(mTimer, &QTimer::timeout, this, &Operation::executeStage);
        mConnected = true;
    }

    mPrepared = false;
    mStart = std::chrono::steady_clock::now();

    mTimer->start(0);
>>>>>>> origin/main
}

void CSMDoc::Operation::appendStage(Stage* stage)
{
    mStages.emplace_back(stage, 0);
}

void CSMDoc::Operation::setDefaultSeverity(Message::Severity severity)
{
    mDefaultSeverity = severity;
}

bool CSMDoc::Operation::hasError() const
{
    return mError;
}

void CSMDoc::Operation::abort()
{
<<<<<<< HEAD
    if (!mStart.has_value())
=======
    if (!mTimer->isActive())
>>>>>>> origin/main
        return;

    mError = true;

    if (mFinalAlways)
    {
        if (mStages.begin() != mStages.end() && mCurrentStage != --mStages.end())
        {
            mCurrentStep = 0;
            mCurrentStage = --mStages.end();
        }
    }
    else
        mCurrentStage = mStages.end();
}

void CSMDoc::Operation::executeStage()
{
    if (!mPrepared)
    {
        prepareStages();
        mPrepared = true;
    }

    Messages messages(mDefaultSeverity);

<<<<<<< HEAD
    const auto batchStart = std::chrono::steady_clock::now();
    static constexpr auto batchBudget = std::chrono::milliseconds(33);

=======
>>>>>>> origin/main
    while (mCurrentStage != mStages.end())
    {
        if (mCurrentStep >= mCurrentStage->second)
        {
            mCurrentStep = 0;
            ++mCurrentStage;
        }
        else
        {
            try
            {
                mCurrentStage->first->perform(mCurrentStep++, messages);
            }
            catch (const std::exception& e)
            {
                emit reportMessage(
                    Message(CSMWorld::UniversalId(), e.what(), "", Message::Severity_SeriousError), mType);
                abort();
            }

            ++mCurrentStepTotal;

            if (std::chrono::steady_clock::now() - batchStart >= batchBudget)
                break;
        }
    }

    emit progress(mCurrentStepTotal, mTotalSteps ? mTotalSteps : 1, mType);

    for (Messages::Iterator iter(messages.begin()); iter != messages.end(); ++iter)
        emit reportMessage(*iter, mType);

    if (mCurrentStage == mStages.end())
    {
        if (mStart.has_value())
        {
            const auto duration = std::chrono::steady_clock::now() - *mStart;
            Log(Debug::Verbose) << operationToString(mType) << " operation is completed in "
                                << std::chrono::duration_cast<std::chrono::duration<double>>(duration).count() << 's';
            mStart.reset();
        }

        operationDone();
<<<<<<< HEAD
        return;
    }

    QMetaObject::invokeMethod(this, &Operation::executeStage, Qt::QueuedConnection);
=======
    }
>>>>>>> origin/main
}

void CSMDoc::Operation::operationDone()
{
<<<<<<< HEAD
    emit done(mType, mError);
}

void CSMDoc::Operation::cleanup()
{
    moveToThread(QCoreApplication::instance()->thread());
=======
    mTimer->stop();
    emit done(mType, mError);
>>>>>>> origin/main
}
