// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "olap/tablet_meta_manager.h"

#include <gen_cpp/olap_file.pb.h>
#include <gtest/gtest-message.h>
#include <gtest/gtest-test-part.h>
#include <json2pb/json_to_pb.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <string>

#include "gtest/gtest_pred_impl.h"
#include "olap/data_dir.h"

using std::string;

namespace doris {
using namespace ErrorCode;

const std::string meta_path = "./be/test/olap/test_data/header_without_inc_rs.txt";

class TabletMetaManagerTest : public testing::Test {
public:
    virtual void SetUp() {
        std::string root_path = "./store";
        EXPECT_TRUE(std::filesystem::create_directory(root_path));
        _data_dir = new (std::nothrow) DataDir(root_path);
        EXPECT_NE(nullptr, _data_dir);
        Status st = _data_dir->init();
        EXPECT_TRUE(st.ok());
        EXPECT_TRUE(std::filesystem::exists(root_path + "/meta"));

        std::ifstream infile(meta_path);
        char buffer[1024];
        while (!infile.eof()) {
            infile.getline(buffer, 1024);
            _json_header = _json_header + buffer + "\n";
        }
        _json_header = _json_header.substr(0, _json_header.size() - 1);
        _json_header = _json_header.substr(0, _json_header.size() - 1);
    }

    virtual void TearDown() {
        delete _data_dir;
        EXPECT_TRUE(std::filesystem::remove_all("./store"));
    }

private:
    DataDir* _data_dir;
    std::string _json_header;
};

TEST_F(TabletMetaManagerTest, TestSaveAndGetAndRemove) {
    const TTabletId tablet_id = 15672;
    const TSchemaHash schema_hash = 567997577;
    TabletMetaPB tablet_meta_pb;
    bool ret = json2pb::JsonToProtoMessage(_json_header, &tablet_meta_pb);
    EXPECT_TRUE(ret);

    std::string meta_binary;
    tablet_meta_pb.SerializeToString(&meta_binary);
    TabletMetaSharedPtr tablet_meta(new TabletMeta());
    Status s = tablet_meta->deserialize(meta_binary);
    EXPECT_EQ(Status::OK(), s);

    s = TabletMetaManager::save(_data_dir, tablet_id, schema_hash, tablet_meta);
    EXPECT_EQ(Status::OK(), s);
    std::string json_meta_read;
    s = TabletMetaManager::get_json_meta(_data_dir, tablet_id, schema_hash, &json_meta_read);
    EXPECT_EQ(Status::OK(), s);
    EXPECT_EQ(_json_header, json_meta_read);
    s = TabletMetaManager::remove(_data_dir, tablet_id, schema_hash);
    EXPECT_EQ(Status::OK(), s);
    TabletMetaSharedPtr meta_read(new TabletMeta());
    s = TabletMetaManager::get_meta(_data_dir, tablet_id, schema_hash, meta_read);
    EXPECT_EQ(Status::Error<META_KEY_NOT_FOUND>(), s);
}

TEST_F(TabletMetaManagerTest, TestLoad) {
    const TTabletId tablet_id = 15672;
    const TSchemaHash schema_hash = 567997577;
    Status s = TabletMetaManager::load_json_meta(_data_dir, meta_path);
    EXPECT_EQ(Status::OK(), s);
    std::string json_meta_read;
    s = TabletMetaManager::get_json_meta(_data_dir, tablet_id, schema_hash, &json_meta_read);
    EXPECT_EQ(Status::OK(), s);
    EXPECT_EQ(_json_header, json_meta_read);
}

} // namespace doris
