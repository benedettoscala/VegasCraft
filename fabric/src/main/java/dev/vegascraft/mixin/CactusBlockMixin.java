package dev.vegascraft.mixin;

import dev.vegascraft.link.Proto;
import dev.vegascraft.world.NvLand;
import dev.vegascraft.world.SkyDig;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.LevelReader;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.CactusBlock;
import net.minecraft.world.level.block.state.BlockState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Redirect;

/** Treat dry New Vegas soil as cactus support, without replacing any world blocks. */
@Mixin(CactusBlock.class)
public abstract class CactusBlockMixin {
    @Redirect(method = "canSurvive", at = @At(value = "INVOKE",
        target = "Lnet/minecraft/world/level/LevelReader;getBlockState(Lnet/minecraft/core/BlockPos;)Lnet/minecraft/world/level/block/state/BlockState;"))
    private BlockState vegascraft$cactusGround(LevelReader queriedLevel, BlockPos query,
            BlockState cactus, LevelReader level, BlockPos pos) {
        BlockState actual = queriedLevel.getBlockState(query);
        if (!actual.isAir() || !query.equals(pos.below())) return actual;
        NvLand.Chunk land = NvLand.at(pos.getX(), pos.getZ());
        if (land == null || !land.known(pos.getX(), pos.getZ())) return actual;
        int material = land.material(pos.getX(), pos.getZ());
        if (material != Proto.DIG_SAND && material != Proto.DIG_DIRT) return actual;
        if ((land.flags(pos.getX(), pos.getZ()) & Proto.LAND_WATER) != 0) return actual;
        double height = land.height(pos.getX(), pos.getZ());
        // Native terrain can cut through the lower half of the cactus's grid cell.
        if (height < pos.getY() - 0.5 || height > pos.getY() + 0.5) return actual;
        if (level instanceof Level world && (SkyDig.isDug(world, land.worldId(), query)
                || SkyDig.isDug(world, land.worldId(), pos))) return actual;
        return Blocks.SAND.defaultBlockState();
    }
}
