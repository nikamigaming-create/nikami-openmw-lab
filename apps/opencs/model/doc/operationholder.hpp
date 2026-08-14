#ifndef CSM_DOC_OPERATIONHOLDER_H
#define CSM_DOC_OPERATIONHOLDER_H

#include <QObject>
#include <QThread>

namespace CSMDoc
{
    class Operation;
    struct Message;

    class OperationHolder : public QObject
    {
        Q_OBJECT

        QThread mThread;
        Operation* mOperation;
<<<<<<< HEAD

    public:
        OperationHolder(QObject* parent, Operation* operation);

        ~OperationHolder();
=======
        bool mRunning;

    public:
        OperationHolder(Operation* operation = nullptr);

        void setOperation(Operation* operation);
>>>>>>> origin/main

        bool isRunning() const;

        void start();

        void abort();

<<<<<<< HEAD
        /// Stop the operation, wait for the thread to finish, and delete the operation.
        /// Safe to call multiple times.
        void quit();
=======
        // Abort and wait until thread has finished.
        void abortAndWait();
>>>>>>> origin/main

    private slots:

        void doneSlot(int type, bool failed);

    signals:

        void progress(int current, int max, int type);

        void reportMessage(const CSMDoc::Message& message, int type);

        void done(int type, bool failed);

        void abortSignal();
    };
}

#endif
