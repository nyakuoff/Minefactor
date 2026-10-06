package dev.minefactor.client.mixin;

import dev.minefactor.link.Proto;
import dev.minefactor.link.SkyLink;

import dev.minefactor.client.SkyDigClient;
import net.minecraft.client.Minecraft;
import net.minecraft.world.InteractionHand;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** Attacking Skyrim's geometry digs into it (SkyDigClient). */
@Mixin(Minecraft.class)
public abstract class MinecraftDigMixin {
	@Inject(method = "startAttack", at = @At("HEAD"), cancellable = true)
	private void minefactor$digStart(CallbackInfoReturnable<Boolean> cir) {
		Minecraft minecraft = (Minecraft) (Object) this;
		if (SkyDigClient.attack(minecraft)) {
			// A swing, not a miss: no miss cooldown before mining the block that appears.
			var held = minecraft.player.getItemInHand(InteractionHand.MAIN_HAND);
			minecraft.player.swing(InteractionHand.MAIN_HAND, held.getAttackAnimation(), false);
			cir.setReturnValue(true);
		} else if (minecraft.hitResult instanceof dev.minefactor.world.SkyClip.SkyrimHitResult result && minecraft.player != null) {
			// Any other surface of the host's world: the host decides what it is and whether it breaks
			// (crates, glass, furniture, ...), from where we hit it and with what.
			var player = minecraft.player;
			var held = player.getItemInHand(InteractionHand.MAIN_HAND);
			float damage = (float) player.getAttributeValue(net.minecraft.world.entity.ai.attributes.Attributes.ATTACK_DAMAGE) * player.getAttackStrengthScale(0.5F);
			int tools = (held.is(net.minecraft.tags.ItemTags.PICKAXES) ? Proto.TOOL_PICKAXE : 0) | (held.is(net.minecraft.tags.ItemTags.AXES) ? Proto.TOOL_AXE : 0)
				| (held.is(net.minecraft.tags.ItemTags.SHOVELS) ? Proto.TOOL_SHOVEL : 0) | (held.is(net.minecraft.tags.ItemTags.HOES) ? Proto.TOOL_HOE : 0)
				| (held.is(net.minecraft.tags.ItemTags.SWORDS) ? Proto.TOOL_SWORD : 0);
			var hit = result.hit;
			SkyLink.pushEvent(Proto.EV_HIT_WORLD, 0, (float) hit.x(), (float) hit.y(), (float) hit.z(), damage, tools,
				dev.minefactor.combat.SkyrimActorEntity.weaponClass(held));
			player.resetAttackStrengthTicker();
			player.swing(InteractionHand.MAIN_HAND, held.getAttackAnimation(), false);
			cir.setReturnValue(true);
		}
	}

	@Inject(method = "continueAttack", at = @At("HEAD"))
	private void minefactor$digHold(boolean down, CallbackInfo ci) {
		if (down) {
			SkyDigClient.attack((Minecraft) (Object) this);
		}
	}
}
