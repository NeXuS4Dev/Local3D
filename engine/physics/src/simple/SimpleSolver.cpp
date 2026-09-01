#include "simple/SimpleSolver.hpp"

#include "local3d/math/Constants.hpp"

#include <algorithm>
#include <cmath>

namespace l3d::physics {
namespace {

/// Velocity of a body at a world space point.
[[nodiscard]] math::Vec3 VelocityAt(const Body& body, math::Vec3 radius) noexcept {
    return body.linearVelocity + math::Cross(body.angularVelocity, radius);
}

/// Applies an equal and opposite impulse at the contact points.
void ApplyContactImpulse(Body& a, math::Vec3 radiusA, Body& b, math::Vec3 radiusB,
                         math::Vec3 impulse) noexcept {
    ApplyLinearImpulse(a, -impulse);
    ApplyAngularImpulse(a, -math::Cross(radiusA, impulse));
    ApplyLinearImpulse(b, impulse);
    ApplyAngularImpulse(b, math::Cross(radiusB, impulse));
}

} // namespace

void SolveVelocities(std::span<SimpleContact> contacts, std::span<Body* const> bodies,
                     const PhysicsSettings& settings) {
    // Bounce is decided once, from how fast the pair was closing when the step
    // started, and then treated as a target velocity.
    for (SimpleContact& contact : contacts) {
        contact.restitutionBias = 0.0f;
        contact.normalImpulse = 0.0f;
        contact.tangentImpulse = 0.0f;
        if (contact.isTrigger) {
            continue;
        }
        Body& a = *bodies[contact.bodyA];
        Body& b = *bodies[contact.bodyB];
        if (!CanMove(a) && !CanMove(b)) {
            continue;
        }
        const math::Vec3 radiusA = contact.point - a.desc.pose.position;
        const math::Vec3 radiusB = contact.point - b.desc.pose.position;
        const math::Vec3 relative = VelocityAt(b, radiusB) - VelocityAt(a, radiusA);
        const f32 approach = math::Dot(relative, contact.normal);
        if (approach >= -settings.restitutionThreshold) {
            continue;
        }
        // The bouncier of the two wins.  Taking the minimum instead - which is
        // what friction does - would mean a superball never bounces on anything
        // that is not itself rubber, because almost every level surface has a
        // restitution of zero.
        const f32 restitution =
            a.desc.restitution > b.desc.restitution ? a.desc.restitution : b.desc.restitution;
        contact.restitutionBias = -restitution * approach;
    }

    // The sliding direction is chosen once per step so that the accumulated
    // tangent impulse below always means the same thing.
    for (SimpleContact& contact : contacts) {
        contact.tangent = math::Vec3::Zero();
        if (contact.isTrigger) {
            continue;
        }
        const Body& a = *bodies[contact.bodyA];
        const Body& b = *bodies[contact.bodyB];
        const math::Vec3 radiusA = contact.point - a.desc.pose.position;
        const math::Vec3 radiusB = contact.point - b.desc.pose.position;
        const math::Vec3 relative = VelocityAt(b, radiusB) - VelocityAt(a, radiusA);
        math::Vec3 tangent = relative - contact.normal * math::Dot(relative, contact.normal);
        const f32 length = math::Length(tangent);
        if (length > math::kEpsilon) {
            contact.tangent = tangent / length;
        }
    }

    for (u32 iteration = 0; iteration < settings.velocityIterations; ++iteration) {
        for (SimpleContact& contact : contacts) {
            if (contact.isTrigger) {
                continue;
            }
            Body& a = *bodies[contact.bodyA];
            Body& b = *bodies[contact.bodyB];
            if (!CanMove(a) && !CanMove(b)) {
                continue;
            }

            const math::Vec3 radiusA = contact.point - a.desc.pose.position;
            const math::Vec3 radiusB = contact.point - b.desc.pose.position;
            const math::Vec3 relative = VelocityAt(b, radiusB) - VelocityAt(a, radiusA);

            // --- Normal impulse -------------------------------------------
            const f32 approachSpeed = math::Dot(relative, contact.normal);
            const f32 normalMass = EffectiveMass(a, radiusA, b, radiusB, contact.normal);
            if (normalMass > math::kEpsilon) {
                // Adding the bias asks the solver to leave the pair separating
                // rather than merely not closing.
                f32 impulse = (contact.restitutionBias - approachSpeed) / normalMass;
                // A contact can push, never pull.  Clamping the *accumulated*
                // impulse rather than this increment is what stops one iteration
                // from undoing the support a previous one established.
                const f32 previous = contact.normalImpulse;
                contact.normalImpulse = std::max(0.0f, previous + impulse);
                impulse = contact.normalImpulse - previous;
                if (impulse > 0.0f) {
                    ApplyContactImpulse(a, radiusA, b, radiusB, contact.normal * impulse);
                }

                // --- Friction ----------------------------------------------
                // Measured along the step's fixed tangent, after the normal
                // impulse, so it opposes the sliding that is actually left.
                const math::Vec3& tangent = contact.tangent;
                const f32 tangentSpeed = math::Dot(VelocityAt(b, radiusB) - VelocityAt(a, radiusA),
                                                   tangent);
                if (tangent != math::Vec3::Zero()) {
                    const f32 tangentMass = EffectiveMass(a, radiusA, b, radiusB, tangent);
                    if (tangentMass > math::kEpsilon) {
                        // Geometric mean: one frictionless surface makes the pair
                        // frictionless, which matches how sliding feels.
                        const f32 friction = std::sqrt(a.desc.friction * b.desc.friction);
                        const f32 limit = friction * contact.normalImpulse;
                        const f32 wanted = contact.tangentImpulse - tangentSpeed / tangentMass;
                        const f32 clamped = wanted < -limit ? -limit
                                                            : (wanted > limit ? limit : wanted);
                        const f32 applied = clamped - contact.tangentImpulse;
                        contact.tangentImpulse = clamped;
                        if (applied != 0.0f) {
                            ApplyContactImpulse(a, radiusA, b, radiusB, tangent * applied);
                        }
                    }
                }
            }
        }
    }
}

void SolvePositions(std::span<SimpleContact> contacts, std::span<Body* const> bodies,
                    const PhysicsSettings& settings) {
    for (SimpleContact& contact : contacts) {
        contact.separated = 0.0f;
    }

    for (u32 iteration = 0; iteration < settings.positionIterations; ++iteration) {
        // The list is sorted by body pair, so the points of one manifold are
        // adjacent.  They have to be corrected as a *group*: every point of a
        // four point box contact reports roughly the same overlap, and applying
        // each one's correction in full pushes the pair apart four times too
        // far.  That is not a small error - it launches a crate clear of the
        // floor it landed on, which then falls back, over-corrects again, and
        // reports an endless Begin/End flicker to anything listening.
        usize index = 0;
        while (index < contacts.size()) {
            SimpleContact& contact = contacts[index];
            usize groupEnd = index + 1;
            f32 penetration = contact.penetration;
            while (groupEnd < contacts.size() && contacts[groupEnd].bodyA == contact.bodyA &&
                   contacts[groupEnd].bodyB == contact.bodyB) {
                penetration = contacts[groupEnd].penetration > penetration
                                  ? contacts[groupEnd].penetration
                                  : penetration;
                ++groupEnd;
            }

            if (!contact.isTrigger) {
                Body& a = *bodies[contact.bodyA];
                Body& b = *bodies[contact.bodyB];
                // Leaving `contactSlop` of overlap alone is what stops two bodies
                // that are merely touching from buzzing, and subtracting
                // `separated` is what stops each iteration from re-correcting the
                // overlap a previous one already removed.
                const f32 overlap = penetration - contact.separated - settings.contactSlop;
                const f32 invMassSum = a.invMass + b.invMass;
                if (overlap > 0.0f && invMassSum > math::kEpsilon &&
                    (CanMove(a) || CanMove(b))) {
                    const f32 correction = (overlap * settings.positionCorrection) / invMassSum;
                    a.desc.pose.position -= contact.normal * (correction * a.invMass);
                    b.desc.pose.position += contact.normal * (correction * b.invMass);
                    contact.separated += overlap * settings.positionCorrection;
                }
            }
            index = groupEnd;
        }
    }
}

} // namespace l3d::physics
