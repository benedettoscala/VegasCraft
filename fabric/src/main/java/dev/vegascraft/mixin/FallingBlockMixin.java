package dev.vegascraft.mixin;

import dev.vegascraft.world.SkyCollision;
import dev.vegascraft.world.NvLand;
import dev.vegascraft.world.SkyDig;
import net.minecraft.core.BlockPos;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.util.RandomSource;
import net.minecraft.world.level.block.FallingBlock;
import net.minecraft.world.level.block.state.BlockState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/** Sand and gravel rest on Skyrim ground instead of falling forever through it. */
@Mixin(FallingBlock.class)
public abstract class FallingBlockMixin {
	@Inject(method = "tick", at = @At("HEAD"), cancellable = true)
	private void skycraft$restOnSkyrim(BlockState state, ServerLevel level, BlockPos pos, RandomSource random, CallbackInfo ci) {
        NvLand.Chunk land = NvLand.at(pos.getX(), pos.getZ());
        boolean landSupport = false;
        if (land != null && land.known(pos.getX(), pos.getZ())) {
            double height = land.height(pos.getX(), pos.getZ());
            landSupport = height >= pos.getY() - 0.5 && height <= pos.getY() + 1.5
                && !SkyDig.isDug(level, land.worldId(), pos)
                && !SkyDig.isDug(level, land.worldId(), pos.below());
        }
		if (SkyCollision.supportsFromBelow(pos) || landSupport) {
			ci.cancel();
		}
	}
}
