"""(TICK-051) RX_VK_ASTC_BATCH: ASTC textures kept as BC1 / BC7 decoded, encoded and copied a group of (level, layer) pieces
at a time. The group must give each piece a scratch layer and an encoder-buffer range of its own, keep the dispatches'
inputs those of the one-piece path, and order decode -> encode -> copy -> next group with full barriers.

python -I -B tests/test_astc_batch.py
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
TEX = ROOT / "gpu" / "vk" / "vk_texture.cpp"


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def groups(sizes, layers=8, cap=(2048 // 4) * (2048 // 4) * 8 * 16, block_bytes=16):
    """The grouping rule of astc_bc_batched, in Python: pieces in order, at most `layers` a group, their blocks in the buffer"""
    out, i0 = [], 0
    while i0 < len(sizes):
        i1, used = i0, 0
        while i1 < len(sizes) and i1 - i0 < layers:
            w, h = sizes[i1]
            b = ((w + 3) // 4) * ((h + 3) // 4) * block_bytes
            if i1 > i0 and used + b > cap:
                break
            used += b
            i1 += 1
        out.append((i0, i1, used))
        i0 = i1
    return out


class AstcBatchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tex = TEX.read_text(encoding="utf-8")
        cls.batch = block(cls.tex, "BcCacheEntry* store) {")
        cls.pieces = block(cls.tex, "                                   uint64_t align) {")
        cls.upload = block(cls.tex, "bool upload_astc_gpu(")

    def test_default_and_gated(self):
        on = block(self.tex, "bool astc_batch_on() {")
        self.assertIn('atoi(getenv("RX_VK_ASTC_BATCH")) != 0 : true;', on, "the batched transcode is not on by default")
        gate = self.upload[self.upload.index("if (astc_batch_on()"):]
        gate = gate[:gate.index("{")]
        for need in ("!tex.bc_resized", "bc_mode(tex.format) != 3", "bc_layer_views[kBcLayers - 1]"):
            self.assertIn(need, gate)
        self.assertLess(self.upload.index("if (!bc_scratch_layout(cmd, tex.bc_resized)) {"),
                        self.upload.index("if (astc_batch_on()"), "the batched path runs before the scratch is made")

    def test_each_piece_its_own_layer_and_range(self):
        b = self.batch
        self.assertEqual(b.count("bc_layer_views[i - i0]"), 2, "decode and encode of a piece use different scratch layers")
        self.assertIn("vkCmdDispatch(cmd, p.gx, p.gy, 1);", b)
        self.assertIn("mode, (uint32_t)blocks}", b, "the encoder writes every piece at the buffer's start")
        self.assertIn("at[i - i0] = blocks * block_bytes;", b)
        self.assertIn("{at[i - i0], 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, p.level, p.layer, 1}", b)
        self.assertIn("const uint64_t block_bytes = mode == 1 ? 8 : 16;", b)

    def test_same_decoder_inputs_as_one_piece(self):
        one = self.upload[self.upload.index("for (size_t k0 = 0; k0 < layers.size();)"):]
        self.assertIn("const uint64_t from = k0 * tail + off;", one)
        self.assertIn("p.from = k * tail + off;", self.pieces)
        for field in ("pc.layer_stride = (uint32_t)tail;", "pc.x_shift = 9 + g.tile_h + g.tile_d;",
                      "pc.block_size = gobs_x << pc.x_shift;", "pc.native_tiling = 1;"):
            self.assertIn(field, self.pieces)
        self.assertIn("VkDescriptorBufferInfo bi{s.buffer, s.offset + p.from, staged - p.from};", self.batch)

    def test_barriers_between_stages(self):
        b = self.batch
        loop = b[b.index("for (size_t i0 = 0; i0 < pieces.size();)"):]
        dec = loop.index("vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, astc_pipe);")
        enc = loop.index("vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, bc17_pipe);")
        cpy = loop.index("vkCmdCopyBufferToImage(")
        bars = [m.start() for m in re.finditer(r"full_barrier\(cmd\);", loop)]
        self.assertEqual(len(bars), 3)
        self.assertTrue(dec < bars[0] < enc < bars[1] < cpy < bars[2], "a stage reads what the one before writes, unordered")

    def test_bc_cache(self):
        cap = block(self.tex, "uint64_t bcc_cap() {")
        self.assertIn(": 1024) << 20;", cap, "the BC cache's default is not 1024 MB")
        self.assertIn("std::max(0, atoi(", cap, "RX_VK_BC_CACHE_MB=0 does not turn it off")
        u = self.upload
        gate = u[u.index("const bool cache_this ="):u.index(";", u.index("const bool cache_this ="))]
        for need in ("bcc_cap()", "astc_batch_on()", "all", "tex.bc", "!tex.bc_resized", "bc_mode(tex.format) != 3"):
            self.assertIn(need, gate)
        key = u[u.index("if (cache_this) {"):u.index("cache_key = h | 1;")]
        for need in ("tex.format", "tex.width << 32 | tex.height", "tex.layers << 32 | tex.levels", "pc.level << 32 | pc.layer",
                     "pc.from", "sizeof pc.pc", "bcc_bytes_hash(h, guest_bytes(tex.gpu + at, n, hs), n)"):
            self.assertIn(need, key, "the cache key leaves out " + need)
        self.assertLess(u.index("if (hit.buf) {"), u.index("const Staging s = staging("), "a hit still stages and decodes")
        self.assertIn("it->second.bytes == bcc_size(tex, pieces)", u)
        self.assertIn("if (cache_key && !alpha) {", u, "a BC1 upload that found alpha (made again as BC7) is stored")
        self.assertIn("vkCmdCopyBuffer(cmd, bc_out, store->buf, 1, &bcp);", self.batch)
        free = block(self.tex, "void bcc_free_done(uint64_t completed) {")
        self.assertIn("if (it->last_value > completed) { ++it; continue; }", free)
        self.assertIn("bcc_free_done(completed);", block(self.tex, "void texture_frame(uint64_t completed) {"))

    def test_views_made_and_given_back(self):
        self.assertIn("vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, j, 1};", block(self.tex, "bool bc_scratch_make() {"))
        idle = block(self.tex, "void bc_scratch_idle() {")
        self.assertIn("for (VkImageView& v : bc_layer_views)", idle)

    def test_grouping_rule(self):
        # a 512 x 512 texture of 10 levels: two groups (8 + 2), the buffer never over its size
        sizes = [(max(1, 512 >> l), max(1, 512 >> l)) for l in range(10)]
        g = groups(sizes)
        self.assertEqual([(a, b) for a, b, _ in g], [(0, 8), (8, 10)])
        # 2048 x 2048 levels fill the buffer: never over it, every piece placed once
        big = [(2048, 2048)] * 20
        g = groups(big)
        self.assertEqual(sum(b - a for a, b, _ in g), 20)
        self.assertTrue(all(u <= (2048 // 4) ** 2 * 8 * 16 for _, _, u in g))
        self.assertIn("if (i1 > i0 && used + b > out_cap) break;", self.batch)
        self.assertIn("i1 - i0 < kBcLayers", self.batch)


if __name__ == "__main__":
    unittest.main()
