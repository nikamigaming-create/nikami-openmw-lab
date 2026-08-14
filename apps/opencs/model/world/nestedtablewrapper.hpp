#ifndef CSM_WOLRD_NESTEDTABLEWRAPPER_H
#define CSM_WOLRD_NESTEDTABLEWRAPPER_H

namespace CSMWorld
{
    struct NestedTableWrapperBase
    {
        virtual ~NestedTableWrapperBase() = default;

<<<<<<< HEAD
        virtual size_t size() const = 0;
=======
        virtual int size() const { return -5; }

        NestedTableWrapperBase() = default;
>>>>>>> origin/main
    };

    template <typename NestedTable>
    struct NestedTableWrapper : public NestedTableWrapperBase
    {
        NestedTable mNestedTable;

        NestedTableWrapper(const NestedTable& nestedTable)
            : mNestedTable(nestedTable)
        {
        }

        ~NestedTableWrapper() override = default;

<<<<<<< HEAD
        size_t size() const override
=======
        int size() const override
>>>>>>> origin/main
        {
            return mNestedTable.size(); // i hope that this will be enough
        }
    };
}
#endif
