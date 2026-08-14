#include "operationholder.hpp"

#include "operation.hpp"

<<<<<<< HEAD
CSMDoc::OperationHolder::OperationHolder(QObject* parent, Operation* operation)
    : QObject(parent)
    , mOperation(operation)
{
=======
CSMDoc::OperationHolder::OperationHolder(Operation* operation)
    : mOperation(nullptr)
    , mRunning(false)
{
    if (operation)
        setOperation(operation);
}

void CSMDoc::OperationHolder::setOperation(Operation* operation)
{
    mOperation = operation;
    mOperation->moveToThread(&mThread);

>>>>>>> origin/main
    connect(mOperation, &Operation::progress, this, &OperationHolder::progress);

    connect(mOperation, &Operation::reportMessage, this, &OperationHolder::reportMessage);

    connect(mOperation, &Operation::done, this, &OperationHolder::doneSlot);

    connect(this, &OperationHolder::abortSignal, mOperation, &Operation::abort);

    connect(&mThread, &QThread::started, mOperation, &Operation::run);
<<<<<<< HEAD

    // When the worker thread finishes, move the operation (and its child QTimer)
    // back to the main thread so it can be safely reused or deleted. This must be
    // a DirectConnection so it runs on the worker thread before it fully exits —
    // moveToThread requires being called from the object's current thread.
    connect(&mThread, &QThread::finished, mOperation, &Operation::cleanup, Qt::DirectConnection);
}

CSMDoc::OperationHolder::~OperationHolder()
{
    quit();
=======
>>>>>>> origin/main
}

bool CSMDoc::OperationHolder::isRunning() const
{
<<<<<<< HEAD
    return mThread.isRunning();
=======
    return mRunning;
>>>>>>> origin/main
}

void CSMDoc::OperationHolder::start()
{
<<<<<<< HEAD
    if (!mOperation || mThread.isRunning())
        return;

    mOperation->moveToThread(&mThread);
=======
    mRunning = true;
>>>>>>> origin/main
    mThread.start();
}

void CSMDoc::OperationHolder::abort()
{
<<<<<<< HEAD
    emit abortSignal();
}

void CSMDoc::OperationHolder::quit()
{
    if (mThread.isRunning())
    {
        abort();
        mThread.quit();
        mThread.wait();
    }

    if (mOperation)
    {
        delete mOperation;
        mOperation = nullptr;
    }
=======
    mRunning = false;
    emit abortSignal();
}

void CSMDoc::OperationHolder::abortAndWait()
{
    if (mRunning)
    {
        mThread.quit();
        mThread.wait();
    }
>>>>>>> origin/main
}

void CSMDoc::OperationHolder::doneSlot(int type, bool failed)
{
<<<<<<< HEAD
    mThread.quit();
    mThread.wait();
=======
    mRunning = false;
    mThread.quit();
>>>>>>> origin/main
    emit done(type, failed);
}
