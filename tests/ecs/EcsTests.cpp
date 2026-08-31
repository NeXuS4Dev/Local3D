// ECS tests: entity lifetime/generations, component storage, iteration and the
// system scheduler.
#include "doctest.h"

#include "local3d/ecs/System.hpp"
#include "local3d/ecs/World.hpp"

#include <string>
#include <vector>

using namespace l3d;
using namespace l3d::ecs;

namespace {

struct Position {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;
};

struct Velocity {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;
};

struct Health {
    i32 points = 100;
};

struct Tag {
    std::string label;
};

} // namespace

TEST_SUITE("ecs.entity") {
    TEST_CASE("entities are created alive and destroyed once") {
        World world;
        const Entity a = world.CreateEntity();
        const Entity b = world.CreateEntity();
        CHECK(a.IsValid());
        CHECK(a != b);
        CHECK(world.IsAlive(a));
        CHECK(world.IsAlive(b));
        CHECK(world.EntityCount() == 2);

        world.DestroyEntity(a);
        CHECK_FALSE(world.IsAlive(a));
        CHECK(world.EntityCount() == 1);
        world.DestroyEntity(a); // Idempotent.
        CHECK(world.EntityCount() == 1);
    }

    TEST_CASE("indices are recycled with a new generation") {
        World world;
        const Entity first = world.CreateEntity();
        world.DestroyEntity(first);
        const Entity recycled = world.CreateEntity();

        CHECK(recycled.index == first.index);
        CHECK(recycled.generation != first.generation);
        CHECK_FALSE(world.IsAlive(first)); // Stale handle must fail.
        CHECK(world.IsAlive(recycled));
    }

    TEST_CASE("null entity is rejected") {
        World world;
        CHECK_FALSE(world.IsAlive(kNullEntity));
        CHECK(world.Get<Position>(kNullEntity) == nullptr);
        CHECK_FALSE(world.Remove<Position>(kNullEntity));
    }

    TEST_CASE("destroying an entity removes its components") {
        World world;
        const Entity entity = world.CreateEntity();
        world.Emplace<Position>(entity, Position{1.0f, 2.0f, 3.0f});
        world.Emplace<Health>(entity);
        CHECK(world.ComponentCount<Position>() == 1);

        world.DestroyEntity(entity);
        CHECK(world.ComponentCount<Position>() == 0);
        CHECK(world.ComponentCount<Health>() == 0);

        const Entity recycled = world.CreateEntity();
        CHECK(world.Get<Position>(recycled) == nullptr);
    }
}

TEST_SUITE("ecs.components") {
    TEST_CASE("emplace, get, has, remove") {
        World world;
        const Entity entity = world.CreateEntity();

        Position& position = world.Emplace<Position>(entity, Position{1.0f, 2.0f, 3.0f});
        CHECK(position.x == 1.0f);
        CHECK(world.Has<Position>(entity));
        CHECK_FALSE(world.Has<Velocity>(entity));

        world.Get<Position>(entity)->y = 42.0f;
        CHECK(world.Get<Position>(entity)->y == 42.0f);

        CHECK(world.Remove<Position>(entity));
        CHECK_FALSE(world.Remove<Position>(entity));
        CHECK_FALSE(world.Has<Position>(entity));
    }

    TEST_CASE("components hold non trivial types") {
        World world;
        const Entity entity = world.CreateEntity();
        world.Emplace<Tag>(entity, Tag{"player"});
        CHECK(world.Get<Tag>(entity)->label == "player");
        world.Get<Tag>(entity)->label = "enemy";
        CHECK(world.Get<Tag>(entity)->label == "enemy");

        world.DestroyEntity(entity);
        CHECK(world.ComponentCount<Tag>() == 0);
    }

    TEST_CASE("component ids are distinct per type") {
        CHECK(ComponentIdOf<Position>() != ComponentIdOf<Velocity>());
        CHECK(ComponentIdOf<Position>() == ComponentIdOf<Position>());
    }

    TEST_CASE("copy components duplicates onto another entity") {
        World world;
        const Entity source = world.CreateEntity();
        const Entity target = world.CreateEntity();
        world.Emplace<Position>(source, Position{5.0f, 6.0f, 7.0f});
        world.Emplace<Tag>(source, Tag{"copied"});

        world.CopyComponents(source, target);
        REQUIRE(world.Get<Position>(target) != nullptr);
        CHECK(world.Get<Position>(target)->x == 5.0f);
        CHECK(world.Get<Tag>(target)->label == "copied");
        CHECK(world.ComponentCount<Tag>() == 2);
    }

    TEST_CASE("many entities keep components addressable") {
        World world;
        std::vector<Entity> entities;
        for (int i = 0; i < 1000; ++i) {
            const Entity entity = world.CreateEntity();
            world.Emplace<Position>(entity, Position{static_cast<f32>(i), 0.0f, 0.0f});
            entities.push_back(entity);
        }
        CHECK(world.EntityCount() == 1000);
        CHECK(world.ComponentCount<Position>() == 1000);
        for (int i = 0; i < 1000; ++i) {
            REQUIRE(world.Get<Position>(entities[static_cast<usize>(i)]) != nullptr);
            CHECK(world.Get<Position>(entities[static_cast<usize>(i)])->x == static_cast<f32>(i));
        }
    }
}

TEST_SUITE("ecs.iteration") {
    TEST_CASE("each visits every component of a type") {
        World world;
        for (int i = 0; i < 5; ++i) {
            const Entity entity = world.CreateEntity();
            world.Emplace<Position>(entity, Position{static_cast<f32>(i), 0.0f, 0.0f});
        }
        f32 sum = 0.0f;
        usize count = 0;
        world.Each<Position>([&sum, &count](Entity entity, Position& position) {
            CHECK(entity.IsValid());
            sum += position.x;
            ++count;
            position.y = 1.0f; // Iteration must allow mutation.
        });
        CHECK(count == 5);
        CHECK(sum == doctest::Approx(10.0f));

        f32 ySum = 0.0f;
        world.Each<Position>([&ySum](Entity, Position& position) { ySum += position.y; });
        CHECK(ySum == doctest::Approx(5.0f));
    }

    TEST_CASE("for each intersects component sets") {
        World world;
        // Three entities: position only, position+velocity, velocity only.
        const Entity positionOnly = world.CreateEntity();
        world.Emplace<Position>(positionOnly, Position{1.0f, 0.0f, 0.0f});

        const Entity both = world.CreateEntity();
        world.Emplace<Position>(both, Position{2.0f, 0.0f, 0.0f});
        world.Emplace<Velocity>(both, Velocity{10.0f, 0.0f, 0.0f});

        const Entity velocityOnly = world.CreateEntity();
        world.Emplace<Velocity>(velocityOnly, Velocity{99.0f, 0.0f, 0.0f});

        usize visited = 0;
        world.ForEach<Position, Velocity>([&visited, both](Entity entity, Position& position,
                                                           Velocity& velocity) {
            CHECK(entity == both);
            position.x += velocity.x;
            ++visited;
        });
        CHECK(visited == 1);
        CHECK(world.Get<Position>(both)->x == doctest::Approx(12.0f));
    }

    TEST_CASE("for each with three components and no matches") {
        World world;
        const Entity entity = world.CreateEntity();
        world.Emplace<Position>(entity);
        world.Emplace<Velocity>(entity);
        usize visited = 0;
        world.ForEach<Position, Velocity, Health>(
            [&visited](Entity, Position&, Velocity&, Health&) { ++visited; });
        CHECK(visited == 0);
    }

    TEST_CASE("clear removes everything") {
        World world;
        for (int i = 0; i < 10; ++i) {
            const Entity entity = world.CreateEntity();
            world.Emplace<Position>(entity);
        }
        world.Clear();
        CHECK(world.EntityCount() == 0);
        CHECK(world.ComponentCount<Position>() == 0);
        CHECK(world.AllEntities().empty());
    }
}

TEST_SUITE("ecs.systems") {
    TEST_CASE("systems run in phase and priority order") {
        std::vector<std::string> order;
        SystemScheduler scheduler;
        scheduler.RegisterSystem<FunctionSystem>(
            "update-low", SystemPhase::Update,
            [&order](SystemContext&) { order.emplace_back("update-low"); }, 0);
        scheduler.RegisterSystem<FunctionSystem>(
            "update-high", SystemPhase::Update,
            [&order](SystemContext&) { order.emplace_back("update-high"); }, 10);
        scheduler.RegisterSystem<FunctionSystem>(
            "pre", SystemPhase::PreUpdate, [&order](SystemContext&) { order.emplace_back("pre"); });

        World world;
        SystemContext context;
        context.world = &world;
        scheduler.ExecuteAll(context);

        REQUIRE(order.size() == 3);
        CHECK(order[0] == "pre");
        CHECK(order[1] == "update-high");
        CHECK(order[2] == "update-low");
    }

    TEST_CASE("disabled systems are skipped and can be found by name") {
        usize runs = 0;
        SystemScheduler scheduler;
        auto& system = scheduler.RegisterSystem<FunctionSystem>(
            "toggle", SystemPhase::Update, [&runs](SystemContext&) { ++runs; });

        World world;
        SystemContext context;
        context.world = &world;
        scheduler.ExecutePhase(SystemPhase::Update, context);
        CHECK(runs == 1);

        system.SetEnabled(false);
        scheduler.ExecutePhase(SystemPhase::Update, context);
        CHECK(runs == 1);

        CHECK(scheduler.Find("toggle") != nullptr);
        CHECK(scheduler.Find("missing") == nullptr);
        CHECK(scheduler.SystemCount() == 1);
    }

    TEST_CASE("a system can integrate motion over frames") {
        World world;
        for (int i = 0; i < 3; ++i) {
            const Entity entity = world.CreateEntity();
            world.Emplace<Position>(entity);
            world.Emplace<Velocity>(entity, Velocity{1.0f, 0.0f, 0.0f});
        }

        SystemScheduler scheduler;
        scheduler.RegisterSystem<FunctionSystem>(
            "integrate", SystemPhase::Update, [](SystemContext& context) {
                context.world->ForEach<Position, Velocity>([&context](Entity, Position& position,
                                                                      Velocity& velocity) {
                    position.x += velocity.x * context.deltaTime;
                });
            });

        SystemContext context;
        context.world = &world;
        context.deltaTime = 0.5f;
        scheduler.ExecutePhase(SystemPhase::Update, context);
        scheduler.ExecutePhase(SystemPhase::Update, context);

        usize checked = 0;
        world.Each<Position>([&checked](Entity, Position& position) {
            CHECK(position.x == doctest::Approx(1.0f));
            ++checked;
        });
        CHECK(checked == 3);
    }

    TEST_CASE("phase names are stable") {
        CHECK(SystemPhaseName(SystemPhase::FixedUpdate) == "FixedUpdate");
    }
}
