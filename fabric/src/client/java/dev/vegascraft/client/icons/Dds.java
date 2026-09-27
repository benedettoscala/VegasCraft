package dev.vegascraft.client.icons;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/** Decodes the top mip level of a DDS image (DXT1, DXT3, DXT5 or uncompressed 32-bit) to ARGB. */
final class Dds {
	final int width, height;
	final int[] argb;

	private Dds(int width, int height, int[] argb) {
		this.width = width;
		this.height = height;
		this.argb = argb;
	}

	static Dds decode(byte[] file) throws IOException {
		ByteBuffer b = ByteBuffer.wrap(file).order(ByteOrder.LITTLE_ENDIAN);
		if (file.length < 128 || b.getInt(0) != 0x20534444) {
			throw new IOException("not a DDS image");
		}
		int height = b.getInt(12), width = b.getInt(16);
		int pfFlags = b.getInt(80), fourCc = b.getInt(84), bits = b.getInt(88);
		if (width <= 0 || height <= 0 || width > 4096 || height > 4096) {
			throw new IOException("bad DDS size " + width + "x" + height);
		}
		int[] out = new int[width * height];
		b.position(128);
		if ((pfFlags & 0x4) != 0) {
			int kind = fourCc == 0x31545844 ? 1 : fourCc == 0x33545844 ? 3 : fourCc == 0x35545844 ? 5 : 0;
			if (kind == 0) {
				throw new IOException("unsupported DDS compression");
			}
			for (int by = 0; by < (height + 3) / 4; by++) {
				for (int bx = 0; bx < (width + 3) / 4; bx++) {
					block(b, kind, out, width, height, bx * 4, by * 4);
				}
			}
		} else if (bits == 32) {
			int rMask = b.getInt(92), gMask = b.getInt(96), bMask = b.getInt(100), aMask = b.getInt(104);
			for (int i = 0; i < out.length; i++) {
				int p = b.getInt();
				out[i] = (channel(p, aMask, 0xFF) << 24) | (channel(p, rMask, 0) << 16) | (channel(p, gMask, 0) << 8) | channel(p, bMask, 0);
			}
		} else {
			throw new IOException("unsupported DDS format");
		}
		return new Dds(width, height, out);
	}

	private static int channel(int pixel, int mask, int otherwise) {
		if (mask == 0) {
			return otherwise;
		}
		int shift = Integer.numberOfTrailingZeros(mask);
		int max = mask >>> shift;
		return (int) (((pixel & mask) >>> shift) * 255L / max);
	}

	private static void block(ByteBuffer b, int kind, int[] out, int width, int height, int x0, int y0) {
		int[] alpha = new int[16];
		java.util.Arrays.fill(alpha, 255);
		if (kind == 3) {
			long bitsA = b.getLong();
			for (int i = 0; i < 16; i++) {
				alpha[i] = (int) ((bitsA >>> (4 * i)) & 0xF) * 17;
			}
		} else if (kind == 5) {
			int a0 = b.get() & 0xFF, a1 = b.get() & 0xFF;
			long bitsA = 0;
			for (int i = 0; i < 6; i++) {
				bitsA |= (long) (b.get() & 0xFF) << (8 * i);
			}
			int[] table = new int[8];
			table[0] = a0;
			table[1] = a1;
			for (int i = 2; i < 8; i++) {
				table[i] = a0 > a1 ? ((8 - i) * a0 + (i - 1) * a1) / 7 : i < 6 ? ((6 - i) * a0 + (i - 1) * a1) / 5 : i == 6 ? 0 : 255;
			}
			for (int i = 0; i < 16; i++) {
				alpha[i] = table[(int) ((bitsA >>> (3 * i)) & 7)];
			}
		}
		int c0 = b.getShort() & 0xFFFF, c1 = b.getShort() & 0xFFFF;
		int indices = b.getInt();
		int[] colors = new int[4];
		colors[0] = rgb565(c0);
		colors[1] = rgb565(c1);
		boolean fourColors = kind != 1 || c0 > c1;
		colors[2] = fourColors ? mix(colors[0], colors[1], 2, 1, 3) : mix(colors[0], colors[1], 1, 1, 2);
		colors[3] = fourColors ? mix(colors[0], colors[1], 1, 2, 3) : 0;
		for (int i = 0; i < 16; i++) {
			int x = x0 + (i & 3), y = y0 + (i >> 2);
			if (x >= width || y >= height) {
				continue;
			}
			int index = (indices >>> (2 * i)) & 3;
			int a = kind == 1 ? (!fourColors && index == 3 ? 0 : 255) : alpha[i];
			out[y * width + x] = (a << 24) | (colors[index] & 0xFFFFFF);
		}
	}

	private static int rgb565(int c) {
		int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, bl = c & 0x1F;
		return ((r * 255 / 31) << 16) | ((g * 255 / 63) << 8) | (bl * 255 / 31);
	}

	private static int mix(int a, int b, int wa, int wb, int div) {
		int r = (((a >> 16) & 0xFF) * wa + ((b >> 16) & 0xFF) * wb) / div;
		int g = (((a >> 8) & 0xFF) * wa + ((b >> 8) & 0xFF) * wb) / div;
		int bl = ((a & 0xFF) * wa + (b & 0xFF) * wb) / div;
		return (r << 16) | (g << 8) | bl;
	}
}
