package dev.vegascraft.combat;

import dev.vegascraft.SkyCraft;
import dev.vegascraft.item.NvItems;
import dev.vegascraft.world.SkyCollision;
import net.minecraft.core.particles.BlockParticleOption;
import net.minecraft.core.particles.ParticleTypes;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.sounds.SoundSource;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.entity.projectile.ProjectileUtil;
import net.minecraft.world.level.ClipContext;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.BlockHitResult;
import net.minecraft.world.phys.EntityHitResult;
import net.minecraft.world.phys.HitResult;
import net.minecraft.world.phys.Vec3;

/**
 * New Vegas fired the player's gun (kInNvShot). Its projectile flies in New Vegas' world, where
 * Minecraft's mobs and blocks don't exist, so Minecraft traces the same shot along the player's
 * look (New Vegas aims with it): the first Minecraft mob in line takes the weapon's New Vegas
 * damage, and a Minecraft block in the way stops the shot with an impact. New Vegas' own
 * geometry (its walls, its ground) stops it too, where Minecraft has a copy of it. New Vegas'
 * actors are left to New Vegas, whose bullet already hits them.
 */
public final class NvShots {
	private static final double RANGE = 160.0;       // blocks
	private static final double SEGMENT = 4.0;       // blocks of ray per triangle query
	private static int logged;
	private static String wallWhy = "";

	private NvShots() {
	}

	/** Server thread. `damage` is per projectile; a shotgun's pellets all go along the one ray. */
	public static void fire(ServerPlayer player, int weaponForm, float damage, int projectiles) {
		// The damage is that of the weapon New Vegas actually fired (it may not be the one Minecraft
		// thinks is in hand, when the two inventories differ); Minecraft's hand must hold a gun.
		if (!player.isAlive() || player.isSpectator() || !NvItems.isNvWielded(player.getMainHandItem())) {
			return;
		}
		ServerLevel level = player.level();
		Vec3 eye = player.getEyePosition();
		Vec3 dir = player.getViewVector(1.0F);
		Vec3 end = eye.add(dir.scale(RANGE));
		BlockHitResult block = level.clip(new ClipContext(eye, end, ClipContext.Block.COLLIDER, ClipContext.Fluid.NONE, player));
		double reach = block.getType() == HitResult.Type.MISS ? RANGE : block.getLocation().distanceTo(eye);
		EntityHitResult entity = ProjectileUtil.getEntityHitResult(level, player, eye, eye.add(dir.scale(reach)),
			player.getBoundingBox().expandTowards(dir.scale(reach)).inflate(1.0), NvShots::target, 0.0F);
		double hitAt = entity != null ? entity.getLocation().distanceTo(eye) : reach;
		double wall = newVegasWall(eye, dir, hitAt);
		String what;
		if (wall < hitAt) {
			what = "New Vegas geometry at " + Math.round(wall) + " blocks (" + wallWhy + ")";
		} else if (entity != null) {
			what = hurt(level, player, entity.getEntity(), dir, Math.max(0.0F, damage) * Math.max(1, projectiles));
		} else if (block.getType() != HitResult.Type.MISS) {
			impact(level, block);
			what = "block " + level.getBlockState(block.getBlockPos()).getBlock().getName().getString();
		} else {
			what = "nothing";
		}
		if (logged++ < 40) {
			SkyCraft.LOG.info("VegasCraft: New Vegas shot ({} x{} damage) hit {}", damage, projectiles, what);
		}
	}

	private static boolean target(Entity e) {
		return e instanceof LivingEntity && e.isAlive() && e.isPickable() && !(e instanceof SkyrimActorEntity) && !(e instanceof Player);
	}

	private static String hurt(ServerLevel level, ServerPlayer player, Entity target, Vec3 dir, float damage) {
		LivingEntity living = (LivingEntity) target;
		DamageSource source = level.damageSources().playerAttack(player);
		living.setInvulnerableTime(0);  // automatic weapons fire faster than Minecraft's hurt cooldown
		boolean hurt = living.hurtServer(level, source, damage);
		if (hurt) {
			living.knockback(0.3, -dir.x, -dir.z, source, damage);
		}
		return target.getName().getString() + (hurt ? " for " + damage : " (no damage)") + (living.isAlive() ? "" : ", killed");
	}

	private static void impact(ServerLevel level, BlockHitResult hit) {
		BlockState state = level.getBlockState(hit.getBlockPos());
		Vec3 p = hit.getLocation();
		level.sendParticles(new BlockParticleOption(ParticleTypes.BLOCK, state), p.x, p.y, p.z, 8, 0.05, 0.05, 0.05, 0.1);
		level.playSound(null, p.x, p.y, p.z, state.getSoundType().getHitSound(), SoundSource.BLOCKS, 0.8F, 1.2F);
	}

	/**
	 * Distance along the ray to New Vegas' geometry, or +inf: its exact collision triangles where
	 * Minecraft has them (around the player; holes dug in them let the shot through), and beyond
	 * that the land's height (a hill between the player and the target).
	 */
	private static double newVegasWall(Vec3 eye, Vec3 dir, double max) {
		java.util.List<dev.vegascraft.world.SkyTri> tris = new java.util.ArrayList<>();
		for (double t0 = 0.0; t0 < max; t0 += SEGMENT) {
			double t1 = Math.min(max, t0 + SEGMENT);
			Vec3 a = eye.add(dir.scale(t0)), b = eye.add(dir.scale(t1));
			tris.clear();
			SkyCollision.trianglesNear(new AABB(a, b).inflate(0.05), tris);
			double best = Double.POSITIVE_INFINITY;
			for (dev.vegascraft.world.SkyTri tri : tris) {
				if (tri.stairHelper) {
					continue;  // New Vegas' invisible stair ramps
				}
				double t = intersect(eye, dir, tri);
				if (t >= Math.max(t0, 0.3) && t <= t1 && t < best) {
					best = t;
				}
			}
			if (best < Double.POSITIVE_INFINITY) {
				wallWhy = "triangle";
				return best;
			}
			for (double t = t0; t < t1; t += 0.5) {
				double x = eye.x + dir.x * t, y = eye.y + dir.y * t, z = eye.z + dir.z * t;
				int bx = (int) Math.floor(x), by = (int) Math.floor(y), bz = (int) Math.floor(z);
				if (SkyCollision.isKnown(bx, by, bz)) {
					continue;  // the triangles there are exact
				}
				double ground = dev.vegascraft.world.NvLand.height(bx, bz);
				if (!Double.isNaN(ground) && y < ground - 0.1) {
					wallWhy = String.format("land %.2f at %.1f %.2f %.1f", ground, x, y, z);
					return t;
				}
			}
		}
		return Double.POSITIVE_INFINITY;
	}

	/** Moller-Trumbore: the ray's distance to the triangle, or -1 for a miss. */
	private static double intersect(Vec3 o, Vec3 d, dev.vegascraft.world.SkyTri t) {
		double e1x = t.bx - t.ax, e1y = t.by - t.ay, e1z = t.bz - t.az;
		double e2x = t.cx - t.ax, e2y = t.cy - t.ay, e2z = t.cz - t.az;
		double px = d.y * e2z - d.z * e2y, py = d.z * e2x - d.x * e2z, pz = d.x * e2y - d.y * e2x;
		double det = e1x * px + e1y * py + e1z * pz;
		if (Math.abs(det) < 1e-9) {
			return -1;
		}
		double inv = 1.0 / det;
		double sx = o.x - t.ax, sy = o.y - t.ay, sz = o.z - t.az;
		double u = (sx * px + sy * py + sz * pz) * inv;
		if (u < 0 || u > 1) {
			return -1;
		}
		double qx = sy * e1z - sz * e1y, qy = sz * e1x - sx * e1z, qz = sx * e1y - sy * e1x;
		double v = (d.x * qx + d.y * qy + d.z * qz) * inv;
		if (v < 0 || u + v > 1) {
			return -1;
		}
		return (e2x * qx + e2y * qy + e2z * qz) * inv;
	}
}
