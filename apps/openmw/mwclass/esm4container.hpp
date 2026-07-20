#ifndef GAME_MWCLASS_ESM4CONTAINER_H
#define GAME_MWCLASS_ESM4CONTAINER_H

#include <components/esm4/loadcont.hpp>
#include <components/misc/rng.hpp>

#include <optional>

#include "../mwworld/containerstore.hpp"
#include "../mwworld/customdata.hpp"
#include "../mwworld/registeredclass.hpp"

#include "esm4base.hpp"

namespace ESM
{
    struct InventoryState;
}

namespace MWWorld
{
    class ESMStore;
}

namespace MWBase
{
    class World;
}

namespace MWClass
{
    class ESM4ContainerStore final : public MWWorld::ContainerStore
    {
        template <class Record>
        bool addInitialRecord(
            const MWWorld::ESMStore& store, const ESM::RefId& id, int count, std::optional<float> condition = {});

        void fillImpl(const ESM4::Container& container, const MWWorld::ESMStore& store,
            Misc::Rng::Generator* prng, int playerLevel, MWBase::World* world);

    public:
        void fill(const ESM4::Container& container, const MWWorld::ESMStore& store);
        void fill(const ESM4::Container& container, const MWWorld::ESMStore& store,
            Misc::Rng::Generator& prng, int playerLevel, MWBase::World* world = nullptr);

        std::unique_ptr<MWWorld::ContainerStore> clone() override;
    };

    class ESM4ContainerCustomData final : public MWWorld::TypedCustomData<ESM4ContainerCustomData>
    {
    public:
        ESM4ContainerStore mStore;

        ESM4ContainerCustomData(const ESM4::Container& container, const MWWorld::ESMStore& store);
        ESM4ContainerCustomData(const ESM4::Container& container, const MWWorld::ESMStore& store,
            Misc::Rng::Generator& prng, int playerLevel, MWBase::World* world);
        explicit ESM4ContainerCustomData(const ESM::InventoryState& inventory);
    };

    class ESM4Container final
        : public MWWorld::RegisteredClass<ESM4Container, ESM4Base<ESM4::Container>>
    {
        friend MWWorld::RegisteredClass<ESM4Container, ESM4Base<ESM4::Container>>;

        ESM4Container();

        void ensureCustomData(const MWWorld::Ptr& ptr) const;
        static ESM4ContainerCustomData& getCustomData(const MWWorld::Ptr& ptr);
        static const ESM4ContainerCustomData& getCustomData(const MWWorld::ConstPtr& ptr);

        MWWorld::Ptr copyToCellImpl(const MWWorld::ConstPtr& ptr, MWWorld::CellStore& cell) const override;

    public:
        std::string_view getName(const MWWorld::ConstPtr& ptr) const override;
        bool hasToolTip(const MWWorld::ConstPtr& ptr) const override;
        MWGui::ToolTipInfo getToolTipInfo(const MWWorld::ConstPtr& ptr, int count) const override;

        std::unique_ptr<MWWorld::Action> activate(
            const MWWorld::Ptr& ptr, const MWWorld::Ptr& actor) const override;

        MWWorld::ContainerStore& getContainerStore(const MWWorld::Ptr& ptr) const override;
        float getCapacity(const MWWorld::Ptr& ptr) const override;
        float getEncumbrance(const MWWorld::Ptr& ptr) const override;
        bool canLock(const MWWorld::ConstPtr& ptr) const override;
        ESM::RefId getScript(const MWWorld::ConstPtr& ptr) const override;

        void readAdditionalState(const MWWorld::Ptr& ptr, const ESM::ObjectState& state) const override;
        void writeAdditionalState(const MWWorld::ConstPtr& ptr, ESM::ObjectState& state) const override;

        bool useAnim() const override;
    };
}

#endif // GAME_MWCLASS_ESM4CONTAINER_H
