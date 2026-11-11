#include "Visual2DUpdateTests.h"
#include "PhantomCore.h"

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>

using namespace PhantomCore;

namespace
{
    struct Probe
    {
        int updates{};
        int returned{};
        int destroyed{};
        bool ownerAvailable{true};
    };

    void Require(bool condition, std::string_view message)
    {
        if (!condition) throw std::runtime_error(std::string(message));
    }

    template<int Tag> class UpdateAction final : public Visual2DComponent
    {
    public:
        explicit UpdateAction(Probe& probe, std::function<void(Visual2DNode&)> action = {})
            : probe_(probe), action_(std::move(action)) {}
        ~UpdateAction() override
        {
            ++probe_.destroyed;
            probe_.ownerAvailable = probe_.ownerAvailable && !Owner().Name().empty();
        }
        void Update(double) override
        {
            ++probe_.updates;
            if (action_) action_(Owner());
            ++probe_.returned;
        }
    private:
        Probe& probe_;
        std::function<void(Visual2DNode&)> action_;
    };

    void TestComponentChanges()
    {
        Probe first, removed, survivor, replacement, child, lateAdded;
        Visual2DCanvas canvas;
        auto& host = canvas.Root().CreateChild("host");
        host.AddComponent<UpdateAction<1>>(first, [&](Visual2DNode& owner)
        {
            Require(owner.RemoveComponent<UpdateAction<2>>(), "remove later component");
            Require(owner.RemoveComponent<UpdateAction<1>>(), "remove current component");
            Require(owner.GetComponent<UpdateAction<1>>() == nullptr &&
                owner.GetComponent<UpdateAction<2>>() == nullptr, "removed components disappear immediately");
            Require(first.destroyed == 0 && removed.destroyed == 0, "component destruction waits for Update");
            owner.AddComponent<UpdateAction<1>>(replacement);
            [&]<std::size_t... Index>(std::index_sequence<Index...>)
            {
                (owner.AddComponent<UpdateAction<static_cast<int>(Index) + 10>>(lateAdded), ...);
            }(std::make_index_sequence<16>{});
            owner.CreateChild("created before child phase").AddComponent<UpdateAction<3>>(child);
        });
        host.AddComponent<UpdateAction<2>>(removed);
        host.AddComponent<UpdateAction<4>>(survivor);
        canvas.Update(0.01);
        Require(first.updates == 1 && first.returned == 1 && first.destroyed == 1 &&
            removed.updates == 0 && removed.destroyed == 1 && survivor.updates == 1,
            "component self/sibling deletion preserves remaining visits");
        Require(replacement.updates == 0 && lateAdded.updates == 0 && child.updates == 1,
            "same component phase additions wait; later child phase additions run now");
        Require(first.ownerAvailable && removed.ownerAvailable, "removed components retain a live Owner");
        canvas.Update(0.01);
        Require(replacement.updates == 1 && lateAdded.updates == 16 && survivor.updates == 2 && child.updates == 2,
            "deferred component addition runs exactly once next Update");
    }

    void TestChildChanges()
    {
        Probe first, skippedComponent, skippedDescendant, sibling, survivor, added;
        Visual2DCanvas canvas;
        auto& host = canvas.Root().CreateChild("host");
        auto& current = host.CreateChild("current");
        auto& later = host.CreateChild("removed sibling");
        const auto laterId = later.Id();
        const auto currentId = current.Id();
        current.AddComponent<UpdateAction<1>>(first, [&](Visual2DNode&)
        {
            Require(canvas.RemoveNode(laterId) && canvas.RemoveNode(currentId), "remove sibling and self node");
            Require(canvas.FindNode(laterId) == nullptr && canvas.FindNode(currentId) == nullptr,
                "removed nodes disappear immediately");
            Require(first.destroyed == 0 && sibling.destroyed == 0, "node destruction waits for Update");
            for (int index = 0; index < 128; ++index)
                host.CreateChild("added sibling").AddComponent<UpdateAction<1>>(added);
        });
        current.AddComponent<UpdateAction<2>>(skippedComponent);
        current.CreateChild("removed descendant").AddComponent<UpdateAction<1>>(skippedDescendant);
        later.AddComponent<UpdateAction<1>>(sibling);
        host.CreateChild("surviving sibling").AddComponent<UpdateAction<1>>(survivor);
        canvas.Update(0.01);
        Require(first.updates == 1 && first.returned == 1 && first.destroyed == 1 &&
            sibling.updates == 0 && sibling.destroyed == 1 && survivor.updates == 1 &&
            skippedComponent.updates == 0 && skippedComponent.destroyed == 1 &&
            skippedDescendant.updates == 0 && skippedDescendant.destroyed == 1,
            "removed subtrees stop; surviving sibling remains valid after vector growth");
        Require(added.updates == 0, "children added during their parent child phase wait");
        canvas.Update(0.01);
        Require(added.updates == 128 && survivor.updates == 2, "added siblings run next Update once each");
    }

    void TestAncestorAndFuturePhase()
    {
        Probe current, skipped, later, futureFirst, futureAdded, futureChild;
        Visual2DCanvas canvas;
        auto& outer = canvas.Root().CreateChild("removed ancestor");
        auto& leaf = outer.CreateChild("current descendant");
        const auto outerId = outer.Id();
        auto& future = canvas.Root().CreateChild("future sibling");
        future.AddComponent<UpdateAction<1>>(futureFirst);
        leaf.AddComponent<UpdateAction<1>>(current, [&](Visual2DNode& owner)
        {
            Require(owner.RemoveComponent<UpdateAction<1>>(), "retire component before its owner subtree");
            Require(canvas.RemoveNode(outerId), "remove ancestor while a descendant is updating");
            Require(current.destroyed == 0, "current callback survives ancestor removal");
            future.AddComponent<UpdateAction<2>>(futureAdded);
            future.CreateChild("future child").AddComponent<UpdateAction<1>>(futureChild);
        });
        leaf.AddComponent<UpdateAction<2>>(skipped);
        outer.CreateChild("removed later descendant").AddComponent<UpdateAction<1>>(later);
        canvas.Update(0.01);
        Require(current.updates == 1 && current.returned == 1 && current.destroyed == 1 &&
            current.ownerAvailable && skipped.updates == 0 && later.updates == 0,
            "ancestor deletion stops remaining descendants and keeps retired component Owner alive");
        Require(futureFirst.updates == 1 && futureAdded.updates == 1 && futureChild.updates == 1,
            "additions before an unvisited node's phase run this Update");
    }

    void TestExceptionCleanup()
    {
        Probe removed, survivor;
        Visual2DCanvas canvas;
        auto& node = canvas.Root().CreateChild("throwing node");
        const auto id = node.Id();
        node.AddComponent<UpdateAction<1>>(removed, [&](Visual2DNode& owner)
        {
            bool recursiveRejected{};
            try { canvas.Update(0.01); }
            catch (const std::logic_error&) { recursiveRejected = true; }
            Require(recursiveRejected, "same Canvas recursive Update is rejected without changing the active pass");
            Require(owner.RemoveComponent<UpdateAction<1>>() && canvas.RemoveNode(id),
                "remove active component and node before throwing");
            throw std::runtime_error("expected update exception");
        });
        canvas.Root().CreateChild("surviving node").AddComponent<UpdateAction<1>>(survivor);
        bool caught{};
        try { canvas.Update(0.01); }
        catch (const std::runtime_error& error)
        { caught = std::string_view(error.what()) == "expected update exception"; }
        Require(caught && removed.destroyed == 1 && removed.ownerAvailable && survivor.updates == 0,
            "exception unwinding destroys retired objects after callbacks leave");
        canvas.Update(0.01);
        Require(survivor.updates == 1, "exception cleanup leaves the next Update usable");
    }

    void TestRetirementQueue()
    {
        Probe removed, immediate;
        Visual2DCanvas canvas;
        constexpr int count = 2048;
        for (int index = 0; index < count; ++index)
        {
            auto& node = canvas.Root().CreateChild("retired node");
            node.AddComponent<UpdateAction<1>>(removed, [&](Visual2DNode& owner)
            { Require(canvas.RemoveNode(owner.Id()), "remove current queued node"); });
        }
        canvas.Update(0.01);
        Require(removed.updates == count && removed.returned == count && removed.destroyed == count,
            "many retired siblings finish callbacks and release without recursive queue destruction");
        auto& node = canvas.Root().CreateChild("outside Update");
        node.AddComponent<UpdateAction<1>>(immediate);
        Require(canvas.RemoveNode(node.Id()) && immediate.destroyed == 1,
            "removal outside Update still destroys immediately");
    }
}

int TestVisual2DUpdates()
{
    try
    {
        TestComponentChanges();
        TestChildChanges();
        TestAncestorAndFuturePhase();
        TestExceptionCleanup();
        TestRetirementQueue();
        std::cout << "Visual2D Update mutation and lifetime tests passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Visual2D Update regression: " << error.what() << '\n';
        return 1;
    }
}
