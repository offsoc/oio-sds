# Copyright (C) 2015-2020 OpenIO SAS, as part of OpenIO SDS
# Copyright (C) 2021-2025 OVH SAS
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 3.0 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library.

import math
import time
from io import BytesIO
from urllib.parse import urlparse

from oio.common.constants import OIO_DB_ENABLED, OIO_DB_FROZEN
from oio.common.exceptions import (
    BadRequest,
    NotFound,
    UnrecoverableContent,
)
from oio.common.fullpath import encode_fullpath
from oio.common.storage_method import parse_chunk_method
from oio.common.utils import cid_from_name
from oio.container.client import ContainerClient
from oio.content.content import ChunksHelper
from oio.content.factory import ContentFactory
from tests.functional.content.test_content import hash_data, hash_stream, random_data
from tests.utils import BaseTestCase, random_str


class TestPlainContent(BaseTestCase):
    def setUp(self):
        super(TestPlainContent, self).setUp()

        if len(self.conf["services"]["rawx"]) < 4:
            self.skipTest("Plain tests needs more than 3 rawx to run")

        self.account = self.conf["account"]
        self.chunk_size = self.conf["chunk_size"]
        self.gridconf = {"namespace": self.ns}
        self.content_factory = ContentFactory(
            self.gridconf, logger=self.logger, watchdog=self.watchdog
        )
        self.container_client = ContainerClient(self.gridconf, logger=self.logger)
        self.blob_client = self.content_factory.blob_client
        self.container_name = "TestPlainContent-%f" % time.time()
        self.container_client.container_create(
            account=self.account, reference=self.container_name
        )
        self.clean_later(self.container_name, self.account)
        self.container_id = cid_from_name(self.account, self.container_name).upper()
        self.content = "%s-%s" % (self.__class__.__name__, random_str(4))
        self.stgpol = "SINGLE"
        self.stgpol_twocopies = "TWOCOPIES"
        self.stgpol_threecopies = "THREECOPIES"

    def _test_create(self, stgpol, data_size):
        data = random_data(data_size)
        content = self.content_factory.new(
            self.container_id, self.content, len(data), stgpol
        )

        content.create(BytesIO(data))

        meta, chunks = self.container_client.content_locate(
            cid=self.container_id, content=content.content_id
        )
        self.assertEqual(meta["hash"], hash_data(data, algorithm="md5"))
        self.assertEqual(meta["length"], str(len(data)))
        self.assertEqual(meta["policy"], stgpol)
        self.assertEqual(meta["name"], self.content)
        _, chunk_params = parse_chunk_method(meta["chunk_method"])

        metachunk_nb = int(math.ceil(float(len(data)) / self.chunk_size))
        if metachunk_nb == 0:
            metachunk_nb = 1  # special case for empty content

        chunks = ChunksHelper(chunks)

        # TODO NO NO NO
        if stgpol == self.stgpol_threecopies:
            nb_copy = 3
        elif stgpol == self.stgpol_twocopies:
            nb_copy = 2
        elif stgpol == self.stgpol:
            nb_copy = 1

        self.assertEqual(len(chunks), metachunk_nb * nb_copy)

        for pos in range(metachunk_nb):
            chunks_at_pos = chunks.filter(pos=pos)
            self.assertEqual(len(chunks_at_pos), nb_copy)

            data_begin = pos * self.chunk_size
            data_end = pos * self.chunk_size + self.chunk_size
            chunk_hash = hash_data(
                data[data_begin:data_end], algorithm=chunk_params["cca"]
            )

            for chunk in chunks_at_pos:
                meta, stream = self.blob_client.chunk_get(chunk.url)
                self.assertEqual(
                    hash_stream(stream, algorithm=chunk_params["cca"]), chunk_hash
                )
                self.assertEqual(meta["content_path"], self.content)
                self.assertEqual(meta["container_id"], self.container_id)
                self.assertEqual(meta["content_id"], meta["content_id"])
                self.assertEqual(meta["chunk_id"], chunk.id)
                self.assertEqual(meta["chunk_pos"], str(pos))
                # Check that chunk data matches chunk hash from xattr
                self.assertEqual(meta["chunk_hash"], chunk_hash)
                # Check that chunk data matches chunk hash from database
                self.assertEqual(chunk.checksum, chunk_hash)
                full_path = encode_fullpath(
                    self.account,
                    self.container_name,
                    self.content,
                    meta["content_version"],
                    meta["content_id"],
                )
                self.assertEqual(meta["full_path"], full_path)

    def test_twocopies_create_0_byte(self):
        self._test_create(self.stgpol_twocopies, 0)

    def test_twocopies_create_1_byte(self):
        self._test_create(self.stgpol_twocopies, 1)

    def test_twocopies_create_chunksize_bytes(self):
        self._test_create(self.stgpol_twocopies, self.chunk_size)

    def test_twocopies_create_chunksize_plus_1_bytes(self):
        self._test_create(self.stgpol_twocopies, self.chunk_size + 1)

    def test_twocopies_create_6294503_bytes(self):
        self._test_create(self.stgpol_twocopies, 6294503)

    def test_single_create_0_byte(self):
        self._test_create(self.stgpol, 0)

    def test_single_create_chunksize_plus_1_bytes(self):
        self._test_create(self.stgpol, self.chunk_size + 1)

    def _new_content(self, stgpol, data, broken_pos_list=None, extra_properties=None):
        if broken_pos_list is None:
            broken_pos_list = []
        old_content = self.content_factory.new(
            self.container_id,
            self.content,
            len(data),
            stgpol,
            extra_properties=extra_properties,
        )

        old_content.create(BytesIO(data))

        broken_chunks_info = {}
        for pos, idx in broken_pos_list:
            c = old_content.chunks.filter(pos=pos)[idx]
            meta, stream = self.blob_client.chunk_get(c.url)
            if pos not in broken_chunks_info:
                broken_chunks_info[pos] = {}
            broken_chunks_info[pos][idx] = {
                "url": c.url,
                "id": c.id,
                "hash": c.checksum,
                "dl_meta": meta,
                "dl_hash": hash_stream(stream),
            }
            self.blob_client.chunk_delete(c.url)

        # get the new structure of the uploaded content
        return (
            self.content_factory.get(self.container_id, old_content.content_id),
            broken_chunks_info,
        )

    def _rebuild_and_check(
        self,
        content,
        broken_chunks_info,
        full_rebuild_pos,
    ):
        rebuild_pos, rebuild_idx = full_rebuild_pos
        rebuild_chunk_info = broken_chunks_info[rebuild_pos][rebuild_idx]
        service_id = urlparse(rebuild_chunk_info["url"]).netloc
        content.rebuild_chunk(
            rebuild_chunk_info["id"],
            service_id=service_id,
        )

        # get the new structure of the content
        rebuilt_content = self.content_factory.get(
            self.container_id, content.content_id
        )

        # find the rebuilt chunk
        for c in rebuilt_content.chunks.filter(pos=rebuild_pos):
            # Ids are the same in plain copy, filter on hosts
            if len(content.chunks.filter(host=c.host)) > 0:
                # not the rebuilt chunk
                # if this chunk is broken, it must not have been rebuilt
                for b_c_i in broken_chunks_info[rebuild_pos].values():
                    if c.url == b_c_i["url"]:
                        self.assertRaises(NotFound, self.blob_client.chunk_get, c.url)
                continue
            meta, stream = self.blob_client.chunk_get(c.url)
            self.assertEqual(meta["chunk_id"], c.id)
            self.assertEqual(hash_stream(stream), rebuild_chunk_info["dl_hash"])
            self.assertEqual(c.checksum, rebuild_chunk_info["hash"])
            self.assertNotEqual(c.url, rebuild_chunk_info["url"])
            self.assertGreaterEqual(
                meta["chunk_mtime"], rebuild_chunk_info["dl_meta"]["chunk_mtime"]
            )
            del meta["chunk_mtime"]
            del rebuild_chunk_info["dl_meta"]["chunk_mtime"]
            del meta["chunk_id"]
            del rebuild_chunk_info["dl_meta"]["chunk_id"]
            self.assertEqual(meta, rebuild_chunk_info["dl_meta"])

    def _test_rebuild(
        self,
        stgpol,
        data_size,
        broken_pos_list,
        full_rebuild_pos,
        extra_properties=None,
    ):
        data = random_data(data_size)
        content, broken_chunks_info = self._new_content(
            stgpol,
            data,
            broken_pos_list,
            extra_properties=extra_properties,
        )

        self._rebuild_and_check(content, broken_chunks_info, full_rebuild_pos)

    def test_2copies_content_0_byte_1broken_rebuild_pos_0_idx_0(self):
        self._test_rebuild(self.stgpol_twocopies, 0, [(0, 0)], (0, 0))

    def test_2copies_content_0_byte_1broken_rebuild_pos_0_idx_0_xtr_props(self):
        self._test_rebuild(
            self.stgpol_twocopies,
            0,
            [(0, 0)],
            (0, 0),
            extra_properties={"foo+2": "bar+2"},
        )

    def test_2copies_content_1_byte_1broken_rebuild_pos_0_idx_1(self):
        self._test_rebuild(self.stgpol_twocopies, 1, [(0, 1)], (0, 1))

    def test_3copies_content_chunksize_bytes_2broken_rebuild_pos_0_idx_1(self):
        if len(self.conf["services"]["rawx"]) <= 3:
            self.skipTest("Need more than 3 rawx")
        self._test_rebuild(
            self.stgpol_threecopies, self.chunk_size, [(0, 0), (0, 1)], (0, 1)
        )

    def test_3copies_content_2xchksize_bytes_2broken_rebuild_pos_1_idx_2(self):
        self._test_rebuild(
            self.stgpol_threecopies, 2 * self.chunk_size, [(1, 0), (1, 2)], (1, 2)
        )

    def test_2copies_content_0_byte_2broken_rebuild_pos_0_idx_0(self):
        self.assertRaises(
            UnrecoverableContent,
            self._test_rebuild,
            self.stgpol_twocopies,
            0,
            [(0, 0), (0, 1)],
            (0, 0),
        )

    def test_rebuild_chunk_in_frozen_container(self):
        data = random_data(self.chunk_size)
        content, broken_chunks_info = self._new_content(
            self.stgpol_twocopies, data, [(0, 0)]
        )
        system = {}
        system["sys.status"] = str(OIO_DB_FROZEN)
        self.container_client.container_set_properties(
            self.account, self.container_name, None, system=system
        )

        try:
            full_rebuild_pos = (0, 0)
            self._rebuild_and_check(content, broken_chunks_info, full_rebuild_pos)
        finally:
            system["sys.status"] = str(OIO_DB_ENABLED)
            self.container_client.container_set_properties(
                self.account, self.container_name, None, system=system
            )

    def _test_fetch(self, stgpol, data_size, broken_pos_list):
        data = random_data(data_size)
        content, _ = self._new_content(stgpol, data, broken_pos_list)

        fetched_data = b"".join(content.fetch())

        self.assertEqual(fetched_data, data)

        for pos, idx in broken_pos_list:
            # check nothing has been rebuilt
            c = content.chunks.filter(pos=pos)[0]
            self.assertRaises(NotFound, self.blob_client.chunk_delete, c.url)

    def test_twocopies_fetch_content_0_byte_without_broken_chunks(self):
        self._test_fetch(self.stgpol_twocopies, 0, [])

    def test_twocopies_fetch_content_0_byte_with_broken_0_0(self):
        self._test_fetch(self.stgpol_twocopies, 0, [(0, 0)])

    def test_twocopies_fetch_content_1_byte_without_broken_chunks(self):
        self._test_fetch(self.stgpol_twocopies, 1, [])

    def test_twocopies_fetch_content_1_byte_with_broken_0_0(self):
        self._test_fetch(self.stgpol_twocopies, 1, [(0, 0)])

    def test_twocopies_fetch_chunksize_bytes_without_broken_chunks(self):
        self._test_fetch(self.stgpol_twocopies, self.chunk_size, [])

    def test_twocopies_fetch_2xchuksize_bytes_with_broken_0_0_and_1_0(self):
        self._test_fetch(self.stgpol_twocopies, self.chunk_size * 2, [(0, 0), (1, 0)])

    def test_twocopies_fetch_content_chunksize_bytes_2_broken_chunks(self):
        data = random_data(self.chunk_size)
        content, _ = self._new_content(self.stgpol_twocopies, data, [(0, 0), (0, 1)])
        gen = content.fetch()
        self.assertRaises(UnrecoverableContent, lambda: next(gen))

    def test_single_fetch_content_1_byte_without_broken_chunks(self):
        self._test_fetch(self.stgpol, 1, [])

    def test_single_fetch_chunksize_bytes_plus_1_without_broken_chunk(self):
        self._test_fetch(self.stgpol, self.chunk_size * 2, [])

    def test_content_spare_with_too_many_locations_known(self):
        for stgpol in (self.stgpol, self.stgpol_twocopies, self.stgpol_threecopies):
            data = random_data(self.chunk_size)
            content, _ = self._new_content(stgpol, data)
            _meta, chunks = self.container_client.content_locate(
                cid=self.container_id, content=content.content_id
            )
            for _ in range(2):
                spare_data = {"notin": chunks, "broken": []}
                try:
                    self.container_client.content_spare(
                        cid=self.container_id,
                        content=content.content_id,
                        data=spare_data,
                        stgpol=stgpol,
                        position=chunks[0]["pos"],
                    )
                except BadRequest as exc:
                    self.assertIn("too many locations already known", exc.message)
                else:
                    self.fail("shouldn't happen")
                chunks += chunks
