// Copyright 2025 The gVisor Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// netinet/in.h must be included before netfilter.h.
// clang-format off
#include <linux/netfilter/nf_tables.h>
#include <netinet/in.h>
#include <linux/netfilter.h>
#include <linux/netlink.h>
// clang-format on

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <tuple>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/strings/str_format.h"
#include "test/syscalls/linux/socket_netlink_netfilter_util.h"
#include "test/syscalls/linux/socket_netlink_util.h"
#include "test/util/file_descriptor.h"
#include "test/util/linux_capability_util.h"
#include "test/util/posix_error.h"
#include "test/util/socket_util.h"
#include "test/util/test_util.h"

// Tests for NETLINK_NETFILTER sockets.

namespace gvisor {
namespace testing {

namespace {

constexpr uint32_t kSeq = 12345;

using ::testing::_;

using SockOptTest = ::testing::TestWithParam<
    std::tuple<int, std::function<bool(int)>, std::string>>;

TEST_P(SockOptTest, GetSockOpt) {
  int sockopt = std::get<0>(GetParam());
  auto verifier = std::get<1>(GetParam());
  std::string verifier_description = std::get<2>(GetParam());

  FileDescriptor fd = ASSERT_NO_ERRNO_AND_VALUE(
      Socket(AF_NETLINK, SOCK_RAW, NETLINK_NETFILTER));

  int res;
  socklen_t len = sizeof(res);

  EXPECT_THAT(getsockopt(fd.get(), SOL_SOCKET, sockopt, &res, &len),
              SyscallSucceeds());

  EXPECT_EQ(len, sizeof(res));
  EXPECT_TRUE(verifier(res)) << absl::StrFormat(
      "getsockopt(%d, SOL_SOCKET, %d, &res, &len) => res=%d was unexpected, "
      "expected %s",
      fd.get(), sockopt, res, verifier_description);
}

std::function<bool(int)> IsPositive() {
  return [](int val) { return val > 0; };
}

std::function<bool(int)> IsEqual(int target) {
  return [target](int val) { return val == target; };
}

INSTANTIATE_TEST_SUITE_P(
    NetlinkNetfilterTest, SockOptTest,
    ::testing::Values(
        std::make_tuple(SO_SNDBUF, IsPositive(), "positive send buffer size"),
        std::make_tuple(SO_RCVBUF, IsPositive(),
                        "positive receive buffer size"),
        std::make_tuple(SO_TYPE, IsEqual(SOCK_RAW),
                        absl::StrFormat("SOCK_RAW (%d)", SOCK_RAW)),
        std::make_tuple(SO_DOMAIN, IsEqual(AF_NETLINK),
                        absl::StrFormat("AF_NETLINK (%d)", AF_NETLINK)),
        std::make_tuple(SO_PROTOCOL, IsEqual(NETLINK_NETFILTER),
                        absl::StrFormat("NETLINK_NETFILTER (%d)",
                                        NETLINK_NETFILTER)),
        std::make_tuple(SO_PASSCRED, IsEqual(0), "0")));

class NetlinkNetfilterTest : public ::testing::Test {
 protected:
  void SetUp() override {}

  // Cleans up any tables created by a test, after it has run.
  void TearDown() override {
    SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
    FileDescriptor fd =
        ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

    std::vector<char> destroy_request_buffer =
        NlReq("deltable req ack unspec").Seq(kSeq).Build();

    ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq,
                                             destroy_request_buffer.data(),
                                             destroy_request_buffer.size()));
  }
};

// Netlink sockets must be SOCK_DGRAM or SOCK_RAW.
TEST_F(NetlinkNetfilterTest, CanCreateSocket) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));
  EXPECT_THAT(fd.get(), SyscallSucceeds());
}

TEST_F(NetlinkNetfilterTest, AddAndAddTableWithDormantFlag) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  const char test_table_name[] = "test_table";
  uint32_t table_flags = NFT_TABLE_F_DORMANT;

  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_request_buffer =
      NlReq("newtable req ack inet")
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  std::vector<char> add_request_buffer_2 =
      NlReq("newtable req ack inet")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .U32Attr(NFTA_TABLE_FLAGS, &table_flags)
          .Build();

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq, add_request_buffer.data(),
                                           add_request_buffer.size()));
  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(
      fd, kSeq + 1, add_request_buffer_2.data(), add_request_buffer_2.size()));
}

TEST_F(NetlinkNetfilterTest, AddAndRetrieveNewTable) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  const char test_table_name[] = "test_tab_add_retrieve";
  uint32_t table_flags = NFT_TABLE_F_DORMANT | NFT_TABLE_F_OWNER;
  uint8_t expected_udata[] = {0x01, 0x02, 0x03, 0x04};
  uint32_t expected_chain_count = 0;
  uint32_t expected_flags = table_flags;
  size_t expected_udata_size = sizeof(expected_udata);
  bool correct_response = false;

  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));
  uint32_t expected_owner = ASSERT_NO_ERRNO_AND_VALUE(NetlinkPortID(fd.get()));

  std::vector<char> add_request_buffer =
      NlReq("newtable req ack inet")
          .Seq(kSeq)
          // Include the null terminator at the end of the string.
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .U32Attr(NFTA_TABLE_FLAGS, &table_flags)
          .RawAttr(NFTA_TABLE_USERDATA, expected_udata, sizeof(expected_udata))
          .Build();

  std::vector<char> get_request_buffer =
      // Don't set NLM_F_ACK here, since the check will be done for every
      // nlmsg received.
      NlReq("gettable req inet")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq, add_request_buffer.data(),
                                           add_request_buffer.size()));
  ASSERT_NO_ERRNO(NetlinkRequestResponse(
      fd, get_request_buffer.data(), get_request_buffer.size(),
      [&](const struct nlmsghdr* hdr) {
        CheckNetfilterTableAttributes({
            .hdr = hdr,
            .test_table_name = test_table_name,
            .expected_chain_count = &expected_chain_count,
            .expected_flags = &expected_flags,
            .expected_owner = &expected_owner,
            .expected_udata = expected_udata,
            .expected_udata_size = &expected_udata_size,
            .skip_handle_check = true,
        });
        correct_response = true;
      },
      false));

  ASSERT_TRUE(correct_response);
}

TEST_F(NetlinkNetfilterTest, ErrGettingTableWithDifferentFamily) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  const char test_table_name[] = "test_tab_different_families";
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_request_buffer_ipv4 =
      NlReq("newtable req ack ipv4")
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  std::vector<char> add_request_buffer_ipv6 =
      NlReq("newtable req ack ipv6")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  std::vector<char> get_request_buffer =
      NlReq("gettable req inet")
          .Seq(kSeq + 2)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq,
                                           add_request_buffer_ipv4.data(),
                                           add_request_buffer_ipv4.size()));
  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq + 1,
                                           add_request_buffer_ipv6.data(),
                                           add_request_buffer_ipv6.size()));
  ASSERT_THAT(NetlinkRequestAckOrError(fd, kSeq + 2, get_request_buffer.data(),
                                       get_request_buffer.size()),
              PosixErrorIs(ENOENT, _));
}

TEST_F(NetlinkNetfilterTest, ErrAddExistingTableWithExclusiveFlag) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  const char test_table_name[] = "err_exclusive";

  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_request_buffer =
      NlReq("newtable req ack inet")
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  std::vector<char> add_request_buffer_2 =
      NlReq("newtable req excl inet")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq, add_request_buffer.data(),
                                           add_request_buffer.size()));
  ASSERT_THAT(
      NetlinkRequestAckOrError(fd, kSeq + 1, add_request_buffer_2.data(),
                               add_request_buffer_2.size()),
      PosixErrorIs(EEXIST, _));
}

TEST_F(NetlinkNetfilterTest, ErrAddExistingTableWithReplaceFlag) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  const char test_table_name[] = "err_replace";

  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_request_buffer =
      NlReq("newtable req ack inet")
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  std::vector<char> add_request_buffer_2 =
      NlReq("newtable req replace inet")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq, add_request_buffer.data(),
                                           add_request_buffer.size()));
  ASSERT_THAT(
      NetlinkRequestAckOrError(fd, kSeq + 1, add_request_buffer_2.data(),
                               add_request_buffer_2.size()),
      PosixErrorIs(ENOTSUP, _));
}

TEST_F(NetlinkNetfilterTest, ErrAddTableWithInvalidFamily) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  uint8_t invalid_family = 255;
  const char test_table_name[] = "unsupported_family_table";

  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_request_buffer =
      NlReq("newtable req")
          .Family(invalid_family)
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  ASSERT_THAT(NetlinkRequestAckOrError(fd, kSeq, add_request_buffer.data(),
                                       add_request_buffer.size()),
              PosixErrorIs(ENOTSUP, _));
}

TEST_F(NetlinkNetfilterTest, ErrAddTableWithUnsupportedFlags) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  uint32_t unsupported_flags = 0xFFFFFFFF;
  const char test_table_name[] = "test_table";

  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_request_buffer =
      NlReq("newtable req inet")
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .U32Attr(NFTA_TABLE_FLAGS, &unsupported_flags)
          .Build();

  ASSERT_THAT(NetlinkRequestAckOrError(fd, kSeq, add_request_buffer.data(),
                                       add_request_buffer.size()),
              PosixErrorIs(ENOTSUP, _));
}

TEST_F(NetlinkNetfilterTest, ErrRetrieveNoSpecifiedNameTable) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));

  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> get_request_buffer =
      NlReq("gettable req ack inet").Seq(kSeq).Build();

  ASSERT_THAT(NetlinkRequestAckOrError(fd, kSeq, get_request_buffer.data(),
                                       get_request_buffer.size()),
              PosixErrorIs(EINVAL, _));
}

TEST_F(NetlinkNetfilterTest, ErrRetrieveNonexistentTable) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  const char test_table_name[] = "undefined_table";

  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> get_request_buffer =
      NlReq("gettable req ack inet")
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  ASSERT_THAT(NetlinkRequestAckOrError(fd, kSeq, get_request_buffer.data(),
                                       get_request_buffer.size()),
              PosixErrorIs(ENOENT, _));
}

TEST_F(NetlinkNetfilterTest, ErrRetrieveTableWithOwnerMismatch) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  const char test_table_name[] = "test_table";
  uint32_t table_flags = NFT_TABLE_F_DORMANT | NFT_TABLE_F_OWNER;
  uint8_t expected_udata[3] = {0x01, 0x02, 0x03};
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));
  FileDescriptor fd_2 =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_request_buffer =
      NlReq("newtable req ack inet")
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .U32Attr(NFTA_TABLE_FLAGS, &table_flags)
          .RawAttr(NFTA_TABLE_USERDATA, expected_udata, sizeof(expected_udata))
          .Build();

  std::vector<char> get_request_buffer =
      NlReq("gettable req ack inet")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq, add_request_buffer.data(),
                                           add_request_buffer.size()));

  ASSERT_THAT(
      NetlinkRequestAckOrError(fd_2, kSeq + 1, get_request_buffer.data(),
                               get_request_buffer.size()),
      PosixErrorIs(EPERM, _));
}

TEST_F(NetlinkNetfilterTest, DeleteExistingTableByName) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  const char test_table_name[] = "test_table_name_delete";
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_request_buffer =
      NlReq("newtable req ack inet")
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  std::vector<char> del_request_buffer =
      NlReq("deltable req ack inet")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq, add_request_buffer.data(),
                                           add_request_buffer.size()));
  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(
      fd, kSeq + 1, del_request_buffer.data(), del_request_buffer.size()));
}

TEST_F(NetlinkNetfilterTest, DeleteTableByHandle) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  // Retrieve the actual table handle from the kernel with a GET request.
  uint64_t expected_handle = 0;
  const char test_table_name[] = "test_table_handle_delete";
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_request_buffer =
      NlReq("newtable req ack inet")
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  std::vector<char> get_request_buffer =
      NlReq("gettable req inet")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq, add_request_buffer.data(),
                                           add_request_buffer.size()));

  // Retrieve the table handle from the kernel.
  ASSERT_NO_ERRNO(NetlinkRequestResponse(
      fd, get_request_buffer.data(), get_request_buffer.size(),
      [&](const struct nlmsghdr* hdr) {
        const nfattr* attr = FindNfAttr(hdr, nullptr, NFTA_TABLE_HANDLE);
        EXPECT_NE(attr, nullptr);
        EXPECT_EQ(attr->nfa_type, NFTA_TABLE_HANDLE);
        EXPECT_EQ(attr->nfa_len - NLA_HDRLEN, sizeof(expected_handle));
        expected_handle = *reinterpret_cast<const uint64_t*>(NFA_DATA(attr));
      },
      false));
  EXPECT_NE(expected_handle, 0);

  std::vector<char> del_request_buffer =
      NlReq("deltable req ack inet")
          .Seq(kSeq + 2)
          .U64Attr(NFTA_TABLE_HANDLE, &expected_handle)
          .Build();

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(
      fd, kSeq + 2, del_request_buffer.data(), del_request_buffer.size()));
}

TEST_F(NetlinkNetfilterTest, ErrDeleteNonexistentTable) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  const char test_table_name[] = "nonexistent_table";

  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> del_request_buffer =
      NlReq("deltable req ack inet")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  ASSERT_THAT(NetlinkRequestAckOrError(fd, kSeq + 1, del_request_buffer.data(),
                                       del_request_buffer.size()),
              PosixErrorIs(ENOENT, _));
}

TEST_F(NetlinkNetfilterTest, DestroyNonexistentTable) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  const char test_table_name[] = "nonexistent_table";

  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> destroy_request_buffer =
      NlReq("destroytable req ack inet")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq + 1,
                                           destroy_request_buffer.data(),
                                           destroy_request_buffer.size()));
}

TEST_F(NetlinkNetfilterTest, DeleteAllTablesUnspecifiedFamily) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  char test_table_name_inet[] = "test_table_inet";
  char test_table_name_bridge[] = "test_table_bridge";

  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_request_buffer =
      NlReq("newtable req ack inet")
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name_inet)
          .Build();

  std::vector<char> add_request_buffer_2 =
      NlReq("newtable req ack bridge")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_TABLE_NAME, test_table_name_bridge)
          .Build();

  std::vector<char> destroy_request_buffer =
      NlReq("deltable req ack unspec").Seq(kSeq + 2).Build();

  std::vector<char> get_request_buffer =
      NlReq("gettable req ack inet")
          .Seq(kSeq + 3)
          .StrAttr(NFTA_TABLE_NAME, test_table_name_inet)
          .Build();

  std::vector<char> get_request_buffer_2 =
      NlReq("gettable req ack bridge")
          .Seq(kSeq + 4)
          .StrAttr(NFTA_TABLE_NAME, test_table_name_bridge)
          .Build();

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq, add_request_buffer.data(),
                                           add_request_buffer.size()));
  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(
      fd, kSeq + 1, add_request_buffer_2.data(), add_request_buffer_2.size()));
  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq + 2,
                                           destroy_request_buffer.data(),
                                           destroy_request_buffer.size()));
  ASSERT_THAT(NetlinkRequestAckOrError(fd, kSeq + 3, get_request_buffer.data(),
                                       get_request_buffer.size()),
              PosixErrorIs(ENOENT, _));
  ASSERT_THAT(
      NetlinkRequestAckOrError(fd, kSeq + 4, get_request_buffer_2.data(),
                               get_request_buffer_2.size()),
      PosixErrorIs(ENOENT, _));
}

TEST_F(NetlinkNetfilterTest, DeleteAllTablesUnspecifiedFamilySpecifiedName) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  char test_table_name_same[] = "test_same_name_table";
  char test_table_name_different[] = "test_different_name_table";

  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_request_buffer_inet =
      NlReq("newtable req ack inet")
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name_same)
          .Build();

  std::vector<char> add_request_buffer_bridge =
      NlReq("newtable req ack bridge")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_TABLE_NAME, test_table_name_same)
          .Build();

  std::vector<char> add_request_buffer_different_bridge =
      NlReq("newtable req ack bridge")
          .Seq(kSeq + 2)
          .StrAttr(NFTA_TABLE_NAME, test_table_name_different)
          .Build();

  std::vector<char> destroy_request_buffer =
      NlReq("deltable req ack unspec")
          .Seq(kSeq + 3)
          .StrAttr(NFTA_TABLE_NAME, test_table_name_same)
          .Build();

  std::vector<char> get_request_buffer_inet =
      NlReq("gettable req ack inet")
          .Seq(kSeq + 4)
          .StrAttr(NFTA_TABLE_NAME, test_table_name_same)
          .Build();

  std::vector<char> get_request_buffer_bridge =
      NlReq("gettable req ack bridge")
          .Seq(kSeq + 5)
          .StrAttr(NFTA_TABLE_NAME, test_table_name_same)
          .Build();

  std::vector<char> get_request_buffer_different =
      NlReq("gettable req bridge")
          .Seq(kSeq + 6)
          .StrAttr(NFTA_TABLE_NAME, test_table_name_different)
          .Build();

  bool correct_response = false;

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq,
                                           add_request_buffer_inet.data(),
                                           add_request_buffer_inet.size()));
  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq + 1,
                                           add_request_buffer_bridge.data(),
                                           add_request_buffer_bridge.size()));
  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(
      fd, kSeq + 2, add_request_buffer_different_bridge.data(),
      add_request_buffer_different_bridge.size()));
  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq + 3,
                                           destroy_request_buffer.data(),
                                           destroy_request_buffer.size()));
  ASSERT_THAT(
      NetlinkRequestAckOrError(fd, kSeq + 4, get_request_buffer_inet.data(),
                               get_request_buffer_inet.size()),
      PosixErrorIs(ENOENT, _));
  ASSERT_THAT(
      NetlinkRequestAckOrError(fd, kSeq + 5, get_request_buffer_bridge.data(),
                               get_request_buffer_bridge.size()),
      PosixErrorIs(ENOENT, _));
  ASSERT_NO_ERRNO(NetlinkRequestResponse(
      fd, get_request_buffer_different.data(),
      get_request_buffer_different.size(),
      [&](const struct nlmsghdr* hdr) {
        const struct nfattr* table_name_attr =
            FindNfAttr(hdr, nullptr, NFTA_TABLE_NAME);
        EXPECT_NE(table_name_attr, nullptr);
        EXPECT_EQ(table_name_attr->nfa_type, NFTA_TABLE_NAME);
        std::string name(
            reinterpret_cast<const char*>(NFA_DATA(table_name_attr)));
        EXPECT_EQ(name, test_table_name_different);
        correct_response = true;
      },
      false));

  ASSERT_TRUE(correct_response);
}

TEST_F(NetlinkNetfilterTest, DeleteAllTablesUnspecifiedNameAndHandle) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  char test_table_name_inet[] = "test_table_inet";
  char test_table_name_bridge[] = "test_table_bridge";

  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_request_buffer =
      NlReq("newtable req ack inet")
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name_inet)
          .Build();

  std::vector<char> add_request_buffer_2 =
      NlReq("newtable req ack bridge")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_TABLE_NAME, test_table_name_bridge)
          .Build();

  std::vector<char> destroy_request_buffer =
      NlReq("deltable req ack unspec").Seq(kSeq + 2).Build();

  std::vector<char> get_request_buffer =
      NlReq("gettable req inet")
          .Seq(kSeq + 3)
          .StrAttr(NFTA_TABLE_NAME, test_table_name_inet)
          .Build();

  std::vector<char> get_request_buffer_2 =
      NlReq("gettable req bridge")
          .Seq(kSeq + 4)
          .StrAttr(NFTA_TABLE_NAME, test_table_name_bridge)
          .Build();

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq, add_request_buffer.data(),
                                           add_request_buffer.size()));
  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(
      fd, kSeq + 1, add_request_buffer_2.data(), add_request_buffer_2.size()));
  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq + 2,
                                           destroy_request_buffer.data(),
                                           destroy_request_buffer.size()));
  ASSERT_THAT(NetlinkRequestAckOrError(fd, kSeq + 3, get_request_buffer.data(),
                                       get_request_buffer.size()),
              PosixErrorIs(ENOENT, _));
  ASSERT_THAT(
      NetlinkRequestAckOrError(fd, kSeq + 4, get_request_buffer_2.data(),
                               get_request_buffer_2.size()),
      PosixErrorIs(ENOENT, _));
}

TEST_F(NetlinkNetfilterTest, ErrNewChainWithNoSpecifiedTableName) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  const char test_table_name[] = "test_table_chain";

  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_table_request_buffer =
      NlReq("newtable req ack inet")
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  std::vector<char> add_chain_request_buffer =
      NlReq("newchain req ack inet").Seq(kSeq + 1).Build();

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq,
                                           add_table_request_buffer.data(),
                                           add_table_request_buffer.size()));
  ASSERT_THAT(
      NetlinkRequestAckOrError(fd, kSeq + 1, add_chain_request_buffer.data(),
                               add_chain_request_buffer.size()),
      PosixErrorIs(EINVAL, _));
}

TEST_F(NetlinkNetfilterTest, ErrNewChainWithNonexistentTable) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  const char test_table_name[] = "test_no_table_chain";

  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_chain_request_buffer =
      NlReq("newchain req ack inet")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_CHAIN_TABLE, test_table_name)
          .Build();

  ASSERT_THAT(
      NetlinkRequestAckOrError(fd, kSeq + 1, add_chain_request_buffer.data(),
                               add_chain_request_buffer.size()),
      PosixErrorIs(ENOENT, _));
}

TEST_F(NetlinkNetfilterTest, ErrNewChainWithNoSpecifiedNameOrHandle) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  const char test_table_name[] = "test_no_name_or_handle_chain";

  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_table_request_buffer =
      NlReq("newtable req ack inet")
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  std::vector<char> add_chain_request_buffer =
      NlReq("newchain req ack inet")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_CHAIN_TABLE, test_table_name)
          .Build();

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq,
                                           add_table_request_buffer.data(),
                                           add_table_request_buffer.size()));
  ASSERT_THAT(
      NetlinkRequestAckOrError(fd, kSeq + 1, add_chain_request_buffer.data(),
                               add_chain_request_buffer.size()),
      PosixErrorIs(EINVAL, _));
}

TEST_F(NetlinkNetfilterTest, ErrNewChainWithPolicySet) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  const char test_table_name[] = "test_table_reg_chain";
  const char test_chain_name[] = "test_chain";
  const uint32_t test_policy = NF_ACCEPT;
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_table_request_buffer =
      NlReq("newtable req ack inet")
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  std::vector<char> add_chain_request_buffer =
      NlReq("newchain req ack inet")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_CHAIN_TABLE, test_table_name)
          .StrAttr(NFTA_CHAIN_NAME, test_chain_name)
          .U32Attr(NFTA_CHAIN_POLICY, &test_policy)
          .Build();

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq,
                                           add_table_request_buffer.data(),
                                           add_table_request_buffer.size()));
  ASSERT_THAT(
      NetlinkRequestAckOrError(fd, kSeq + 1, add_chain_request_buffer.data(),
                               add_chain_request_buffer.size()),
      PosixErrorIs(ENOTSUP, _));
}

TEST_F(NetlinkNetfilterTest, ErrNewBaseChainWithInvalidPolicy) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  const char test_table_name[] = "test_table_chain_hook";
  const char test_chain_name[] = "test_chain_bad_policy";
  const uint32_t test_policy = 1 << 3;
  const uint8_t test_hook = NF_INET_PRE_ROUTING;
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_table_request_buffer =
      NlReq("newtable req ack inet")
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  std::vector<char> add_chain_request_buffer =
      NlReq("newchain req ack inet")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_CHAIN_TABLE, test_table_name)
          .StrAttr(NFTA_CHAIN_NAME, test_chain_name)
          .U32Attr(NFTA_CHAIN_POLICY, &test_policy)
          .U8Attr(NFTA_CHAIN_HOOK, &test_hook)
          .Build();

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq,
                                           add_table_request_buffer.data(),
                                           add_table_request_buffer.size()));
  ASSERT_THAT(
      NetlinkRequestAckOrError(fd, kSeq + 1, add_chain_request_buffer.data(),
                               add_chain_request_buffer.size()),
      PosixErrorIs(EINVAL, _));
}

TEST_F(NetlinkNetfilterTest, ErrNewBaseChainWithInvalidFlags) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  const char test_table_name[] = "test_table_chain_hook";
  const char test_chain_name[] = "test_chain_bad_policy";
  const uint32_t test_policy = NF_ACCEPT;
  const uint8_t test_hook = NF_INET_PRE_ROUTING;
  // Only NFT_CHAIN_BASE, NFT_CHAIN_HW_OFFLOAD, and NFT_CHAIN_BINDING are
  // valid flags that should be set by users.
  const uint32_t test_chain_flags = 1 << 3;
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_table_request_buffer =
      NlReq("newtable req ack inet")
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  std::vector<char> add_chain_request_buffer =
      NlReq("newchain req ack inet")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_CHAIN_TABLE, test_table_name)
          .StrAttr(NFTA_CHAIN_NAME, test_chain_name)
          .U32Attr(NFTA_CHAIN_POLICY, &test_policy)
          .U8Attr(NFTA_CHAIN_HOOK, &test_hook)
          .U32Attr(NFTA_CHAIN_FLAGS, &test_chain_flags)
          .Build();

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq,
                                           add_table_request_buffer.data(),
                                           add_table_request_buffer.size()));
  ASSERT_THAT(
      NetlinkRequestAckOrError(fd, kSeq + 1, add_chain_request_buffer.data(),
                               add_chain_request_buffer.size()),
      PosixErrorIs(ENOTSUP, _));
}

TEST_F(NetlinkNetfilterTest,
       ErrNewBaseChainWithMalformedHookDataMissingPriority) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  const char test_table_name[] = "test_table_chain_hook";
  const char test_chain_name[] = "test_chain_bad_policy";
  const uint32_t test_policy = NF_ACCEPT;
  const uint32_t test_hook_num = NF_INET_PRE_ROUTING;
  const uint32_t test_chain_flags = NFT_CHAIN_BASE;
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_table_request_buffer =
      NlReq("newtable req ack inet")
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  std::vector<char> nested_hook_data =
      NlNestedAttr().U32Attr(NFTA_HOOK_HOOKNUM, &test_hook_num).Build();

  std::vector<char> add_chain_request_buffer =
      NlReq("newchain req ack inet")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_CHAIN_TABLE, test_table_name)
          .StrAttr(NFTA_CHAIN_NAME, test_chain_name)
          .U32Attr(NFTA_CHAIN_POLICY, &test_policy)
          .RawAttr(NFTA_CHAIN_HOOK, nested_hook_data.data(),
                   nested_hook_data.size())
          .U32Attr(NFTA_CHAIN_FLAGS, &test_chain_flags)
          .Build();

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq,
                                           add_table_request_buffer.data(),
                                           add_table_request_buffer.size()));
  ASSERT_THAT(
      NetlinkRequestAckOrError(fd, kSeq + 1, add_chain_request_buffer.data(),
                               add_chain_request_buffer.size()),
      PosixErrorIs(ENOENT, _));
}

TEST_F(NetlinkNetfilterTest,
       ErrNewBaseChainWithMalformedHookDataMissingHookNum) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  const char test_table_name[] = "test_table_chain_hook";
  const char test_chain_name[] = "test_chain_bad_policy";
  const uint32_t test_policy = NF_ACCEPT;
  const uint32_t test_hook_priority = 10;
  const uint32_t test_chain_flags = NFT_CHAIN_BASE;
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_table_request_buffer =
      NlReq("newtable req ack inet")
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  std::vector<char> nested_hook_data =
      NlNestedAttr().U32Attr(NFTA_HOOK_PRIORITY, &test_hook_priority).Build();

  std::vector<char> add_chain_request_buffer =
      NlReq("newchain req ack inet")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_CHAIN_TABLE, test_table_name)
          .StrAttr(NFTA_CHAIN_NAME, test_chain_name)
          .U32Attr(NFTA_CHAIN_POLICY, &test_policy)
          .RawAttr(NFTA_CHAIN_HOOK, nested_hook_data.data(),
                   nested_hook_data.size())
          .U32Attr(NFTA_CHAIN_FLAGS, &test_chain_flags)
          .Build();

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq,
                                           add_table_request_buffer.data(),
                                           add_table_request_buffer.size()));
  ASSERT_THAT(
      NetlinkRequestAckOrError(fd, kSeq + 1, add_chain_request_buffer.data(),
                               add_chain_request_buffer.size()),
      PosixErrorIs(ENOENT, _));
}

TEST_F(NetlinkNetfilterTest, ErrNewBaseChainWithInvalidChainType) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  const char test_table_name[] = "test_table_chain_hook";
  const char test_chain_name[] = "test_chain_bad_policy";
  const char test_chain_type_name[] = "test_chain_type_invalid";
  const uint32_t test_policy = NF_ACCEPT;
  const uint32_t test_hook_num = NF_INET_PRE_ROUTING;
  const uint32_t test_hook_priority = 10;
  const uint32_t test_chain_flags = NFT_CHAIN_BASE;
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_table_request_buffer =
      NlReq("newtable req ack inet")
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  std::vector<char> nested_hook_data =
      NlNestedAttr()
          .U32Attr(NFTA_HOOK_HOOKNUM, &test_hook_num)
          .U32Attr(NFTA_HOOK_PRIORITY, &test_hook_priority)
          .StrAttr(NFTA_CHAIN_TYPE, test_chain_type_name)
          .Build();

  std::vector<char> add_chain_request_buffer =
      NlReq("newchain req ack inet")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_CHAIN_TABLE, test_table_name)
          .StrAttr(NFTA_CHAIN_NAME, test_chain_name)
          .U32Attr(NFTA_CHAIN_POLICY, &test_policy)
          .RawAttr(NFTA_CHAIN_HOOK, nested_hook_data.data(),
                   nested_hook_data.size())
          .U32Attr(NFTA_CHAIN_FLAGS, &test_chain_flags)
          .Build();

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq,
                                           add_table_request_buffer.data(),
                                           add_table_request_buffer.size()));
  ASSERT_THAT(
      NetlinkRequestAckOrError(fd, kSeq + 1, add_chain_request_buffer.data(),
                               add_chain_request_buffer.size()),
      PosixErrorIs(ENOENT, _));
}

TEST_F(NetlinkNetfilterTest,
       ErrNewBaseChainWithUnsupportedFamilyChainTypePair) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  const char test_table_name[] = "test_table_chain_hook";
  const char test_chain_name[] = "test_chain_bad_policy";
  const char test_chain_type_name[] = "route";
  const uint32_t test_policy = NF_ACCEPT;
  const uint32_t test_hook_num = NF_INET_PRE_ROUTING;
  const uint32_t test_hook_priority = 10;
  const uint32_t test_chain_flags = NFT_CHAIN_BASE;
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_table_request_buffer =
      NlReq("newtable req ack arp")
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  std::vector<char> nested_hook_data =
      NlNestedAttr()
          .U32Attr(NFTA_HOOK_HOOKNUM, &test_hook_num)
          .U32Attr(NFTA_HOOK_PRIORITY, &test_hook_priority)
          .StrAttr(NFTA_CHAIN_TYPE, test_chain_type_name)
          .Build();

  std::vector<char> add_chain_request_buffer =
      NlReq("newchain req ack arp")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_CHAIN_TABLE, test_table_name)
          .StrAttr(NFTA_CHAIN_NAME, test_chain_name)
          .U32Attr(NFTA_CHAIN_POLICY, &test_policy)
          .RawAttr(NFTA_CHAIN_HOOK, nested_hook_data.data(),
                   nested_hook_data.size())
          .U32Attr(NFTA_CHAIN_FLAGS, &test_chain_flags)
          .Build();

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq,
                                           add_table_request_buffer.data(),
                                           add_table_request_buffer.size()));
  ASSERT_THAT(
      NetlinkRequestAckOrError(fd, kSeq + 1, add_chain_request_buffer.data(),
                               add_chain_request_buffer.size()),
      PosixErrorIs(ENOTSUP, _));
}

TEST_F(NetlinkNetfilterTest, ErrNewNATBaseChainWithInvalidPriority) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  const char test_table_name[] = "test_table_chain_hook";
  const char test_chain_name[] = "test_chain_bad_policy";
  const char test_chain_type_name[] = "nat";
  const uint32_t test_policy = NF_ACCEPT;
  const uint32_t test_hook_num = NF_INET_PRE_ROUTING;
  const uint32_t test_hook_priority = -250;
  const uint32_t test_chain_flags = NFT_CHAIN_BASE;
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_table_request_buffer =
      NlReq("newtable req ack inet")
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  std::vector<char> nested_hook_data =
      NlNestedAttr()
          .U32Attr(NFTA_HOOK_HOOKNUM, &test_hook_num)
          .U32Attr(NFTA_HOOK_PRIORITY, &test_hook_priority)
          .StrAttr(NFTA_CHAIN_TYPE, test_chain_type_name)
          .Build();

  std::vector<char> add_chain_request_buffer =
      NlReq("newchain req ack inet")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_CHAIN_TABLE, test_table_name)
          .StrAttr(NFTA_CHAIN_NAME, test_chain_name)
          .U32Attr(NFTA_CHAIN_POLICY, &test_policy)
          .RawAttr(NFTA_CHAIN_HOOK, nested_hook_data.data(),
                   nested_hook_data.size())
          .U32Attr(NFTA_CHAIN_FLAGS, &test_chain_flags)
          .Build();

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq,
                                           add_table_request_buffer.data(),
                                           add_table_request_buffer.size()));
  ASSERT_THAT(
      NetlinkRequestAckOrError(fd, kSeq + 1, add_chain_request_buffer.data(),
                               add_chain_request_buffer.size()),
      PosixErrorIs(ENOTSUP, _));
}

TEST_F(NetlinkNetfilterTest, ErrNewNetDevBaseChainUnsupported) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  const char test_table_name[] = "test_table_chain_hook";
  const char test_chain_name[] = "test_chain_bad_policy";
  const char test_chain_type_name[] = "filter";
  const uint32_t test_policy = NF_ACCEPT;
  const uint32_t test_hook_num = NF_NETDEV_INGRESS;
  const uint32_t test_hook_priority = 10;
  const uint32_t test_chain_flags = NFT_CHAIN_BASE;
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_table_request_buffer =
      NlReq("newtable req ack netdev")
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  std::vector<char> nested_hook_data =
      NlNestedAttr()
          .U32Attr(NFTA_HOOK_HOOKNUM, &test_hook_num)
          .U32Attr(NFTA_HOOK_PRIORITY, &test_hook_priority)
          .StrAttr(NFTA_CHAIN_TYPE, test_chain_type_name)
          .Build();

  std::vector<char> add_chain_request_buffer =
      NlReq("newchain req ack netdev")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_CHAIN_TABLE, test_table_name)
          .StrAttr(NFTA_CHAIN_NAME, test_chain_name)
          .U32Attr(NFTA_CHAIN_POLICY, &test_policy)
          .RawAttr(NFTA_CHAIN_HOOK, nested_hook_data.data(),
                   nested_hook_data.size())
          .U32Attr(NFTA_CHAIN_FLAGS, &test_chain_flags)
          .Build();

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq,
                                           add_table_request_buffer.data(),
                                           add_table_request_buffer.size()));
  ASSERT_THAT(
      NetlinkRequestAckOrError(fd, kSeq + 1, add_chain_request_buffer.data(),
                               add_chain_request_buffer.size()),
      PosixErrorIs(ENOTSUP, _));
}

TEST_F(NetlinkNetfilterTest, ErrNewInetBaseChainAtIngressUnsupported) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  const char test_table_name[] = "test_table_chain_hook";
  const char test_chain_name[] = "test_chain_bad_policy";
  const char test_chain_type_name[] = "filter";
  const uint32_t test_policy = NF_ACCEPT;
  const uint32_t test_hook_num = NF_INET_INGRESS;
  const uint32_t test_hook_priority = 10;
  const uint32_t test_chain_flags = NFT_CHAIN_BASE;
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_table_request_buffer =
      NlReq("newtable req ack inet")
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  std::vector<char> nested_hook_data =
      NlNestedAttr()
          .U32Attr(NFTA_HOOK_HOOKNUM, &test_hook_num)
          .U32Attr(NFTA_HOOK_PRIORITY, &test_hook_priority)
          .StrAttr(NFTA_CHAIN_TYPE, test_chain_type_name)
          .Build();

  std::vector<char> add_chain_request_buffer =
      NlReq("newchain req ack inet")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_CHAIN_TABLE, test_table_name)
          .StrAttr(NFTA_CHAIN_NAME, test_chain_name)
          .U32Attr(NFTA_CHAIN_POLICY, &test_policy)
          .RawAttr(NFTA_CHAIN_HOOK, nested_hook_data.data(),
                   nested_hook_data.size())
          .U32Attr(NFTA_CHAIN_FLAGS, &test_chain_flags)
          .Build();

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq,
                                           add_table_request_buffer.data(),
                                           add_table_request_buffer.size()));
  ASSERT_THAT(
      NetlinkRequestAckOrError(fd, kSeq + 1, add_chain_request_buffer.data(),
                               add_chain_request_buffer.size()),
      PosixErrorIs(ENOTSUP, _));
}

TEST_F(NetlinkNetfilterTest, ErrNewBaseChainWithUnsupportedChainCounters) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  const char test_table_name[] = "test_table_chain_hook";
  const char test_chain_name[] = "test_chain_bad_policy";
  const char test_chain_type_name[] = "filter";
  const uint32_t test_policy = NF_ACCEPT;
  const uint32_t test_hook_num = NF_INET_INGRESS;
  const uint32_t test_hook_priority = 10;
  const uint32_t test_chain_flags = NFT_CHAIN_BASE;
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_table_request_buffer =
      NlReq("newtable req ack inet")
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  std::vector<char> nested_hook_data =
      NlNestedAttr()
          .U32Attr(NFTA_HOOK_HOOKNUM, &test_hook_num)
          .U32Attr(NFTA_HOOK_PRIORITY, &test_hook_priority)
          .StrAttr(NFTA_CHAIN_TYPE, test_chain_type_name)
          .Build();

  std::vector<char> add_chain_request_buffer =
      NlReq("newchain req ack inet")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_CHAIN_TABLE, test_table_name)
          .StrAttr(NFTA_CHAIN_NAME, test_chain_name)
          .U32Attr(NFTA_CHAIN_POLICY, &test_policy)
          .RawAttr(NFTA_CHAIN_HOOK, nested_hook_data.data(),
                   nested_hook_data.size())
          .U32Attr(NFTA_CHAIN_FLAGS, &test_chain_flags)
          .RawAttr(NFTA_CHAIN_COUNTERS, nullptr, 0)
          .Build();

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq,
                                           add_table_request_buffer.data(),
                                           add_table_request_buffer.size()));
  ASSERT_THAT(
      NetlinkRequestAckOrError(fd, kSeq + 1, add_chain_request_buffer.data(),
                               add_chain_request_buffer.size()),
      PosixErrorIs(ENOTSUP, _));
}

TEST_F(NetlinkNetfilterTest, ErrChainWithBaseChainFlagSet) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  const char test_table_name[] = "test_table_chain_hook";
  const char test_chain_name[] = "test_chain_bad_policy";
  const uint32_t test_chain_flags = NFT_CHAIN_BASE;
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_table_request_buffer =
      NlReq("newtable req ack inet")
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  std::vector<char> add_chain_request_buffer =
      NlReq("newchain req ack inet")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_CHAIN_TABLE, test_table_name)
          .StrAttr(NFTA_CHAIN_NAME, test_chain_name)
          .U32Attr(NFTA_CHAIN_FLAGS, &test_chain_flags)
          .Build();

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq,
                                           add_table_request_buffer.data(),
                                           add_table_request_buffer.size()));
  ASSERT_THAT(
      NetlinkRequestAckOrError(fd, kSeq + 1, add_chain_request_buffer.data(),
                               add_chain_request_buffer.size()),
      PosixErrorIs(EINVAL, _));
}

TEST_F(NetlinkNetfilterTest, ErrChainWithHardwareOffloadFlagSet) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  const char test_table_name[] = "test_table_chain_hook";
  const char test_chain_name[] = "test_chain_bad_policy";
  const uint32_t test_chain_flags = NFT_CHAIN_HW_OFFLOAD;
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_table_request_buffer =
      NlReq("newtable req ack inet")
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  std::vector<char> add_chain_request_buffer =
      NlReq("newchain req ack inet")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_CHAIN_TABLE, test_table_name)
          .StrAttr(NFTA_CHAIN_NAME, test_chain_name)
          .U32Attr(NFTA_CHAIN_FLAGS, &test_chain_flags)
          .Build();

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq,
                                           add_table_request_buffer.data(),
                                           add_table_request_buffer.size()));
  ASSERT_THAT(
      NetlinkRequestAckOrError(fd, kSeq + 1, add_chain_request_buffer.data(),
                               add_chain_request_buffer.size()),
      PosixErrorIs(ENOTSUP, _));
}

TEST_F(NetlinkNetfilterTest, ErrChainWithNoNameAndChainBindingFlagNotSet) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  const char test_table_name[] = "test_table_chain_hook";
  const uint32_t test_chain_flags = 0;
  const uint32_t test_chain_id = 1;
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_table_request_buffer =
      NlReq("newtable req ack inet")
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  std::vector<char> add_chain_request_buffer =
      NlReq("newchain req ack inet")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_CHAIN_TABLE, test_table_name)
          .U32Attr(NFTA_CHAIN_FLAGS, &test_chain_flags)
          .U32Attr(NFTA_CHAIN_ID, &test_chain_id)
          .Build();

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq,
                                           add_table_request_buffer.data(),
                                           add_table_request_buffer.size()));
  ASSERT_THAT(
      NetlinkRequestAckOrError(fd, kSeq + 1, add_chain_request_buffer.data(),
                               add_chain_request_buffer.size()),
      PosixErrorIs(EINVAL, _));
}

TEST_F(NetlinkNetfilterTest, ErrUpdateChain) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  const char test_table_name[] = "test_table_chain_hook";
  const char test_chain_name[] = "test_chain_invalid_update";
  const uint32_t test_chain_flags = 0;
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_table_request_buffer =
      NlReq("newtable req ack inet")
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  std::vector<char> add_chain_request_buffer =
      NlReq("newchain req ack inet")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_CHAIN_TABLE, test_table_name)
          .U32Attr(NFTA_CHAIN_FLAGS, &test_chain_flags)
          .StrAttr(NFTA_CHAIN_NAME, test_chain_name)
          .Build();

  std::vector<char> update_chain_request_buffer =
      NlReq("newchain req ack inet")
          .Seq(kSeq + 2)
          .StrAttr(NFTA_CHAIN_TABLE, test_table_name)
          .U32Attr(NFTA_CHAIN_FLAGS, &test_chain_flags)
          .StrAttr(NFTA_CHAIN_NAME, test_chain_name)
          .Build();

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq,
                                           add_table_request_buffer.data(),
                                           add_table_request_buffer.size()));
  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq + 1,
                                           add_chain_request_buffer.data(),
                                           add_chain_request_buffer.size()));

  ASSERT_THAT(
      NetlinkRequestAckOrError(fd, kSeq + 2, update_chain_request_buffer.data(),
                               update_chain_request_buffer.size()),
      PosixErrorIs(ENOTSUP, _));
}

TEST_F(NetlinkNetfilterTest, AddChainWithNoNameAndChainIdAttributeSet) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  const char test_table_name[] = "test_table_chain_hook";
  const uint32_t test_chain_flags = NFT_CHAIN_BINDING;
  const uint32_t test_chain_id = 2;
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_table_request_buffer =
      NlReq("newtable req ack inet")
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  std::vector<char> add_chain_request_buffer =
      NlReq("newchain req ack inet")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_CHAIN_TABLE, test_table_name)
          .U32Attr(NFTA_CHAIN_FLAGS, &test_chain_flags)
          .U32Attr(NFTA_CHAIN_ID, &test_chain_id)
          .Build();

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq,
                                           add_table_request_buffer.data(),
                                           add_table_request_buffer.size()));
  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq + 1,
                                           add_chain_request_buffer.data(),
                                           add_chain_request_buffer.size()));
}

TEST_F(NetlinkNetfilterTest, AddChainWithName) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  const char test_table_name[] = "test_table_chain_hook";
  const char test_chain_name[] = "test_chain_name";
  const uint32_t test_chain_flags = 0;
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_table_request_buffer =
      NlReq("newtable req ack inet")
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  std::vector<char> add_chain_request_buffer =
      NlReq("newchain req ack inet")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_CHAIN_TABLE, test_table_name)
          .U32Attr(NFTA_CHAIN_FLAGS, &test_chain_flags)
          .StrAttr(NFTA_CHAIN_NAME, test_chain_name)
          .Build();

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq,
                                           add_table_request_buffer.data(),
                                           add_table_request_buffer.size()));
  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq + 1,
                                           add_chain_request_buffer.data(),
                                           add_chain_request_buffer.size()));
}

TEST_F(NetlinkNetfilterTest, AddBaseChainWithDropPolicy) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  const char test_table_name[] = "test_table_chain_hook";
  const char test_chain_name[] = "test_chain_bad_policy";
  const char test_chain_type_name[] = "filter";
  const uint32_t test_policy = NF_DROP;
  const uint32_t test_hook_num = NF_INET_PRE_ROUTING;
  const uint32_t test_hook_priority = 0;
  const uint32_t test_chain_flags = NFT_CHAIN_BASE;
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_table_request_buffer =
      NlReq("newtable req ack inet")
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  std::vector<char> nested_hook_data =
      NlNestedAttr()
          .U32Attr(NFTA_HOOK_HOOKNUM, &test_hook_num)
          .U32Attr(NFTA_HOOK_PRIORITY, &test_hook_priority)
          .StrAttr(NFTA_CHAIN_TYPE, test_chain_type_name)
          .Build();
  std::vector<char> add_chain_request_buffer =
      NlReq("newchain req ack inet")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_CHAIN_TABLE, test_table_name)
          .StrAttr(NFTA_CHAIN_NAME, test_chain_name)
          .U32Attr(NFTA_CHAIN_POLICY, &test_policy)
          .RawAttr(NFTA_CHAIN_HOOK, nested_hook_data.data(),
                   nested_hook_data.size())
          .U32Attr(NFTA_CHAIN_FLAGS, &test_chain_flags)
          .Build();

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq,
                                           add_table_request_buffer.data(),
                                           add_table_request_buffer.size()));
  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq + 1,
                                           add_chain_request_buffer.data(),
                                           add_chain_request_buffer.size()));
}

TEST_F(NetlinkNetfilterTest, ErrGetChainWithDumpFlagSet) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  const char test_table_name[] = "test_table_chain_hook";
  const char test_chain_name[] = "test_chain_dump_fail";
  const uint32_t test_chain_flags = 0;
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_table_request_buffer =
      NlReq("newtable req ack inet")
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  std::vector<char> add_chain_request_buffer =
      NlReq("newchain req ack inet")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_CHAIN_TABLE, test_table_name)
          .U32Attr(NFTA_CHAIN_FLAGS, &test_chain_flags)
          .StrAttr(NFTA_CHAIN_NAME, test_chain_name)
          .Build();

  std::vector<char> get_chain_request_buffer =
      NlReq("getchain req ack dump inet")
          .Seq(kSeq + 2)
          .StrAttr(NFTA_CHAIN_TABLE, test_table_name)
          .StrAttr(NFTA_CHAIN_NAME, test_chain_name)
          .Build();

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq,
                                           add_table_request_buffer.data(),
                                           add_table_request_buffer.size()));
  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq + 1,
                                           add_chain_request_buffer.data(),
                                           add_chain_request_buffer.size()));
  ASSERT_THAT(
      NetlinkRequestAckOrError(fd, kSeq + 2, get_chain_request_buffer.data(),
                               get_chain_request_buffer.size()),
      PosixErrorIs(ENOTSUP, _));
}

TEST_F(NetlinkNetfilterTest, ErrGetChainWithNoTableName) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  const char test_table_name[] = "test_table_chain_hook";
  const char test_chain_name[] = "test_chain_no_table_name";
  const uint32_t test_chain_flags = 0;
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_table_request_buffer =
      NlReq("newtable req ack inet")
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  std::vector<char> add_chain_request_buffer =
      NlReq("newchain req ack inet")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_CHAIN_TABLE, test_table_name)
          .U32Attr(NFTA_CHAIN_FLAGS, &test_chain_flags)
          .StrAttr(NFTA_CHAIN_NAME, test_chain_name)
          .Build();

  std::vector<char> get_chain_request_buffer =
      NlReq("getchain req ack inet")
          .Seq(kSeq + 2)
          .StrAttr(NFTA_CHAIN_NAME, test_chain_name)
          .Build();

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq,
                                           add_table_request_buffer.data(),
                                           add_table_request_buffer.size()));
  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq + 1,
                                           add_chain_request_buffer.data(),
                                           add_chain_request_buffer.size()));
  ASSERT_THAT(
      NetlinkRequestAckOrError(fd, kSeq + 2, get_chain_request_buffer.data(),
                               get_chain_request_buffer.size()),
      PosixErrorIs(EINVAL, _));
}

TEST_F(NetlinkNetfilterTest, ErrGetChainWithNoChainName) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  const char test_table_name[] = "test_table_chain_hook";
  const char test_chain_name[] = "test_chain_no_chain_name";
  const uint32_t test_chain_flags = 0;
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_table_request_buffer =
      NlReq("newtable req ack inet")
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  std::vector<char> add_chain_request_buffer =
      NlReq("newchain req ack inet")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_CHAIN_TABLE, test_table_name)
          .U32Attr(NFTA_CHAIN_FLAGS, &test_chain_flags)
          .StrAttr(NFTA_CHAIN_NAME, test_chain_name)
          .Build();

  std::vector<char> get_chain_request_buffer =
      NlReq("getchain req ack inet")
          .Seq(kSeq + 2)
          .StrAttr(NFTA_CHAIN_TABLE, test_table_name)
          .Build();

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq,
                                           add_table_request_buffer.data(),
                                           add_table_request_buffer.size()));
  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq + 1,
                                           add_chain_request_buffer.data(),
                                           add_chain_request_buffer.size()));
  ASSERT_THAT(
      NetlinkRequestAckOrError(fd, kSeq + 2, get_chain_request_buffer.data(),
                               get_chain_request_buffer.size()),
      PosixErrorIs(EINVAL, _));
}

TEST_F(NetlinkNetfilterTest, GetChain) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  const char test_table_name[] = "test_table_chain";
  const char test_chain_name[] = "test_chain";
  uint8_t test_user_data[] = {0x01, 0x02, 0x03, 0x04};
  size_t expected_udata_size = sizeof(test_user_data);
  const uint32_t test_chain_flags = NFT_CHAIN_BINDING;
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_table_request_buffer =
      NlReq("newtable req ack inet")
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  std::vector<char> add_chain_request_buffer =
      NlReq("newchain req ack inet")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_CHAIN_TABLE, test_table_name)
          .U32Attr(NFTA_CHAIN_FLAGS, &test_chain_flags)
          .StrAttr(NFTA_CHAIN_NAME, test_chain_name)
          .RawAttr(NFTA_CHAIN_USERDATA, test_user_data, expected_udata_size)
          .Build();

  uint32_t expected_use = 0;
  std::vector<char> get_chain_request_buffer =
      NlReq("getchain req inet")
          .Seq(kSeq + 2)
          .StrAttr(NFTA_CHAIN_TABLE, test_table_name)
          .StrAttr(NFTA_CHAIN_NAME, test_chain_name)
          .Build();

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq,
                                           add_table_request_buffer.data(),
                                           add_table_request_buffer.size()));
  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq + 1,
                                           add_chain_request_buffer.data(),
                                           add_chain_request_buffer.size()));

  ASSERT_NO_ERRNO(NetlinkRequestResponse(
      fd, get_chain_request_buffer.data(), get_chain_request_buffer.size(),
      [&](const struct nlmsghdr* hdr) {
        CheckNetfilterChainAttributes({
            .hdr = hdr,
            .expected_table_name = test_table_name,
            .expected_chain_name = test_chain_name,
            .expected_flags = &test_chain_flags,
            .expected_use = &expected_use,
            .expected_udata = test_user_data,
            .expected_udata_size = &expected_udata_size,
            .skip_handle_check = true,
        });
      },
      false));
}

TEST_F(NetlinkNetfilterTest, GetBaseChain) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  const char test_table_name[] = "test_table_chain";
  const char test_chain_name[] = "test_base_chain";
  const char test_chain_type_name[] = "filter";
  const uint32_t test_policy = NF_ACCEPT;
  const uint32_t test_hook_num = NF_INET_PRE_ROUTING;
  const uint32_t test_hook_priority = 10;
  uint8_t test_user_data[] = {0x01, 0x02, 0x03, 0x04};
  size_t expected_udata_size = sizeof(test_user_data);
  const uint32_t test_chain_flags = NFT_CHAIN_BASE;
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_table_request_buffer =
      NlReq("newtable req ack inet")
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  std::vector<char> nested_hook_data =
      NlNestedAttr()
          .U32Attr(NFTA_HOOK_HOOKNUM, &test_hook_num)
          .U32Attr(NFTA_HOOK_PRIORITY, &test_hook_priority)
          .StrAttr(NFTA_CHAIN_TYPE, test_chain_type_name)
          .Build();

  std::vector<char> add_chain_request_buffer =
      NlReq("newchain req ack inet")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_CHAIN_TABLE, test_table_name)
          .StrAttr(NFTA_CHAIN_NAME, test_chain_name)
          .U32Attr(NFTA_CHAIN_POLICY, &test_policy)
          .RawAttr(NFTA_CHAIN_HOOK, nested_hook_data.data(),
                   nested_hook_data.size())
          .U32Attr(NFTA_CHAIN_FLAGS, &test_chain_flags)
          .RawAttr(NFTA_CHAIN_USERDATA, test_user_data, expected_udata_size)
          .Build();

  uint32_t expected_use = 0;
  std::vector<char> get_chain_request_buffer =
      NlReq("getchain req inet")
          .Seq(kSeq + 2)
          .StrAttr(NFTA_CHAIN_TABLE, test_table_name)
          .StrAttr(NFTA_CHAIN_NAME, test_chain_name)
          .Build();

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq,
                                           add_table_request_buffer.data(),
                                           add_table_request_buffer.size()));
  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq + 1,
                                           add_chain_request_buffer.data(),
                                           add_chain_request_buffer.size()));

  ASSERT_NO_ERRNO(NetlinkRequestResponse(
      fd, get_chain_request_buffer.data(), get_chain_request_buffer.size(),
      [&](const struct nlmsghdr* hdr) {
        CheckNetfilterChainAttributes({
            .hdr = hdr,
            .expected_table_name = test_table_name,
            .expected_chain_name = test_chain_name,
            .expected_policy = &test_policy,
            .expected_chain_type = test_chain_type_name,
            .expected_flags = &test_chain_flags,
            .expected_use = &expected_use,
            .expected_udata = test_user_data,
            .expected_udata_size = &expected_udata_size,
            .skip_handle_check = true,
        });
      },
      false));
}

TEST_F(NetlinkNetfilterTest, ErrDeleteChainWithNoTableNameSpecified) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  const char test_table_name[] = "test_table_chains";
  const char test_chain_name[] = "test_chain_no_table_name";
  const char test_chain_type_name[] = "filter";
  const uint32_t test_policy = NF_DROP;
  const uint32_t test_hook_num = NF_INET_PRE_ROUTING;
  const uint32_t test_hook_priority = 0;
  const uint32_t test_chain_flags = NFT_CHAIN_BASE;
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_table_request_buffer =
      NlReq("newtable req ack inet")
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  std::vector<char> nested_hook_data =
      NlNestedAttr()
          .U32Attr(NFTA_HOOK_HOOKNUM, &test_hook_num)
          .U32Attr(NFTA_HOOK_PRIORITY, &test_hook_priority)
          .StrAttr(NFTA_CHAIN_TYPE, test_chain_type_name)
          .Build();
  std::vector<char> add_chain_request_buffer =
      NlReq("newchain req ack inet")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_CHAIN_TABLE, test_table_name)
          .StrAttr(NFTA_CHAIN_NAME, test_chain_name)
          .U32Attr(NFTA_CHAIN_POLICY, &test_policy)
          .RawAttr(NFTA_CHAIN_HOOK, nested_hook_data.data(),
                   nested_hook_data.size())
          .U32Attr(NFTA_CHAIN_FLAGS, &test_chain_flags)
          .Build();

  std::vector<char> delete_chain_request_buffer =
      NlReq("delchain req ack inet")
          .Seq(kSeq + 2)
          .StrAttr(NFTA_CHAIN_NAME, test_chain_name)
          .Build();

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq,
                                           add_table_request_buffer.data(),
                                           add_table_request_buffer.size()));
  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq + 1,
                                           add_chain_request_buffer.data(),
                                           add_chain_request_buffer.size()));
  ASSERT_THAT(
      NetlinkRequestAckOrError(fd, kSeq + 2, delete_chain_request_buffer.data(),
                               delete_chain_request_buffer.size()),
      PosixErrorIs(EINVAL, _));
}

TEST_F(NetlinkNetfilterTest, ErrDeleteNonexistentChain) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  const char test_table_name[] = "test_table_chains";
  const char test_chain_name[] = "test_chain_nonexistent";
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_table_request_buffer =
      NlReq("newtable req ack inet")
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  std::vector<char> delete_chain_request_buffer =
      NlReq("delchain req ack inet")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .StrAttr(NFTA_CHAIN_NAME, test_chain_name)
          .Build();

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq,
                                           add_table_request_buffer.data(),
                                           add_table_request_buffer.size()));
  ASSERT_THAT(
      NetlinkRequestAckOrError(fd, kSeq + 1, delete_chain_request_buffer.data(),
                               delete_chain_request_buffer.size()),
      PosixErrorIs(ENOENT, _));
}

TEST_F(NetlinkNetfilterTest, ErrDeleteChainWithChainBindingFlagSet) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  const char test_table_name[] = "test_table_chains";
  const char test_chain_name[] = "test_chain_binding_set";
  const uint32_t test_chain_flags = NFT_CHAIN_BINDING;
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_table_request_buffer =
      NlReq("newtable req ack inet")
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  std::vector<char> add_chain_request_buffer =
      NlReq("newchain req ack inet")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_CHAIN_TABLE, test_table_name)
          .StrAttr(NFTA_CHAIN_NAME, test_chain_name)
          .U32Attr(NFTA_CHAIN_FLAGS, &test_chain_flags)
          .Build();

  std::vector<char> delete_chain_request_buffer =
      NlReq("delchain req ack inet")
          .Seq(kSeq + 2)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .StrAttr(NFTA_CHAIN_NAME, test_chain_name)
          .Build();

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq,
                                           add_table_request_buffer.data(),
                                           add_table_request_buffer.size()));
  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq + 1,
                                           add_chain_request_buffer.data(),
                                           add_chain_request_buffer.size()));
  ASSERT_THAT(
      NetlinkRequestAckOrError(fd, kSeq + 2, delete_chain_request_buffer.data(),
                               delete_chain_request_buffer.size()),
      PosixErrorIs(ENOTSUP, _));
}

TEST_F(NetlinkNetfilterTest, DestroyNonexistentChain) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  const char test_table_name[] = "test_table_chains";
  const char test_chain_name[] = "test_chain_nonexistent";
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_table_request_buffer =
      NlReq("newtable req ack inet")
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  std::vector<char> delete_chain_request_buffer =
      NlReq("destroychain req ack inet")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .StrAttr(NFTA_CHAIN_NAME, test_chain_name)
          .Build();

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq,
                                           add_table_request_buffer.data(),
                                           add_table_request_buffer.size()));
  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq + 1,
                                           delete_chain_request_buffer.data(),
                                           delete_chain_request_buffer.size()));
}

TEST_F(NetlinkNetfilterTest, DeleteBaseChain) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  const char test_table_name[] = "test_table_chains";
  const char test_chain_name[] = "test_chain_delete_base_chain";
  const char test_chain_type_name[] = "filter";
  const uint32_t test_policy = NF_DROP;
  const uint32_t test_hook_num = NF_INET_PRE_ROUTING;
  const uint32_t test_hook_priority = 0;
  const uint32_t test_chain_flags = NFT_CHAIN_BASE;
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_table_request_buffer =
      NlReq("newtable req ack inet")
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  std::vector<char> nested_hook_data =
      NlNestedAttr()
          .U32Attr(NFTA_HOOK_HOOKNUM, &test_hook_num)
          .U32Attr(NFTA_HOOK_PRIORITY, &test_hook_priority)
          .StrAttr(NFTA_CHAIN_TYPE, test_chain_type_name)
          .Build();
  std::vector<char> add_chain_request_buffer =
      NlReq("newchain req ack inet")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_CHAIN_TABLE, test_table_name)
          .StrAttr(NFTA_CHAIN_NAME, test_chain_name)
          .U32Attr(NFTA_CHAIN_POLICY, &test_policy)
          .RawAttr(NFTA_CHAIN_HOOK, nested_hook_data.data(),
                   nested_hook_data.size())
          .U32Attr(NFTA_CHAIN_FLAGS, &test_chain_flags)
          .Build();

  std::vector<char> delete_chain_request_buffer =
      NlReq("delchain req ack inet")
          .Seq(kSeq + 2)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .StrAttr(NFTA_CHAIN_NAME, test_chain_name)
          .Build();

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq,
                                           add_table_request_buffer.data(),
                                           add_table_request_buffer.size()));
  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq + 1,
                                           add_chain_request_buffer.data(),
                                           add_chain_request_buffer.size()));
  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq + 2,
                                           delete_chain_request_buffer.data(),
                                           delete_chain_request_buffer.size()));
}

TEST_F(NetlinkNetfilterTest, DeleteBaseChainByHandle) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  const char test_table_name[] = "test_table_chains";
  const char test_chain_name[] = "test_chain_delete_base_chain";
  const char test_chain_type_name[] = "filter";
  const uint32_t test_policy = NF_DROP;
  const uint32_t test_hook_num = NF_INET_PRE_ROUTING;
  const uint32_t test_hook_priority = 0;
  const uint32_t test_chain_flags = NFT_CHAIN_BASE;
  uint64_t chain_handle = 0;
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_table_request_buffer =
      NlReq("newtable req ack inet")
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  std::vector<char> nested_hook_data =
      NlNestedAttr()
          .U32Attr(NFTA_HOOK_HOOKNUM, &test_hook_num)
          .U32Attr(NFTA_HOOK_PRIORITY, &test_hook_priority)
          .StrAttr(NFTA_CHAIN_TYPE, test_chain_type_name)
          .Build();
  std::vector<char> add_chain_request_buffer =
      NlReq("newchain req ack inet")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_CHAIN_TABLE, test_table_name)
          .StrAttr(NFTA_CHAIN_NAME, test_chain_name)
          .U32Attr(NFTA_CHAIN_POLICY, &test_policy)
          .RawAttr(NFTA_CHAIN_HOOK, nested_hook_data.data(),
                   nested_hook_data.size())
          .U32Attr(NFTA_CHAIN_FLAGS, &test_chain_flags)
          .Build();

  std::vector<char> get_chain_request_buffer =
      NlReq("getchain req inet")
          .Seq(kSeq + 2)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .StrAttr(NFTA_CHAIN_NAME, test_chain_name)
          .Build();

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq,
                                           add_table_request_buffer.data(),
                                           add_table_request_buffer.size()));
  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq + 1,
                                           add_chain_request_buffer.data(),
                                           add_chain_request_buffer.size()));
  ASSERT_NO_ERRNO(NetlinkRequestResponse(
      fd, get_chain_request_buffer.data(), get_chain_request_buffer.size(),
      [&](const struct nlmsghdr* hdr) {
        const struct nfattr* chain_handle_attr =
            FindNfAttr(hdr, nullptr, NFTA_CHAIN_HANDLE);
        ASSERT_NE(chain_handle_attr, nullptr);
        chain_handle =
            *(reinterpret_cast<uint64_t*>(NFA_DATA(chain_handle_attr)));
      },
      false));

  ASSERT_NE(chain_handle, 0);
  std::vector<char> delete_chain_request_buffer =
      NlReq("delchain req ack inet")
          .Seq(kSeq + 3)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .U64Attr(NFTA_CHAIN_HANDLE, &chain_handle)
          .Build();
  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq + 3,
                                           delete_chain_request_buffer.data(),
                                           delete_chain_request_buffer.size()));
}

TEST_F(NetlinkNetfilterTest, ErrRetrieveTableWithOwnerMismatchUnboundSocket) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  const char test_table_name[] = "test_table";
  uint32_t table_flags = NFT_TABLE_F_DORMANT | NFT_TABLE_F_OWNER;
  uint8_t expected_udata[3] = {0x01, 0x02, 0x03};
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));
  FileDescriptor fd_2 = ASSERT_NO_ERRNO_AND_VALUE(
      Socket(AF_NETLINK, SOCK_RAW, NETLINK_NETFILTER));

  std::vector<char> add_request_buffer =
      NlReq("newtable req ack inet")
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .U32Attr(NFTA_TABLE_FLAGS, &table_flags)
          .RawAttr(NFTA_TABLE_USERDATA, expected_udata, sizeof(expected_udata))
          .Build();

  std::vector<char> get_request_buffer =
      NlReq("gettable req ack inet")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq, add_request_buffer.data(),
                                           add_request_buffer.size()));

  ASSERT_THAT(
      NetlinkRequestAckOrError(fd_2, kSeq + 1, get_request_buffer.data(),
                               get_request_buffer.size()),
      PosixErrorIs(EPERM, _));
}

TEST_F(NetlinkNetfilterTest, AddTableWithUnboundSocket) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  const char test_table_name[] = "test_table";
  uint32_t table_flags = NFT_TABLE_F_DORMANT | NFT_TABLE_F_OWNER;
  uint32_t expected_port_id = 0;
  uint8_t expected_udata[3] = {0x01, 0x02, 0x03};
  FileDescriptor fd = ASSERT_NO_ERRNO_AND_VALUE(
      Socket(AF_NETLINK, SOCK_RAW, NETLINK_NETFILTER));
  bool correct_response = false;

  std::vector<char> add_request_buffer =
      NlReq("newtable req ack inet")
          .Seq(kSeq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .U32Attr(NFTA_TABLE_FLAGS, &table_flags)
          .RawAttr(NFTA_TABLE_USERDATA, expected_udata, sizeof(expected_udata))
          .Build();

  std::vector<char> get_request_buffer =
      NlReq("gettable req inet")
          .Seq(kSeq + 1)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();

  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq, add_request_buffer.data(),
                                           add_request_buffer.size()));

  ASSERT_NO_ERRNO(NetlinkRequestResponse(
      fd, get_request_buffer.data(), get_request_buffer.size(),
      [&](const struct nlmsghdr* hdr) {
        const struct nfattr* owner_attr =
            FindNfAttr(hdr, nullptr, NFTA_TABLE_OWNER);
        EXPECT_NE(owner_attr, nullptr);
        uint32_t owner = *(reinterpret_cast<uint32_t*>(NFA_DATA(owner_attr)));
        EXPECT_NE(owner, 0);
        expected_port_id = owner;
        correct_response = true;
      },
      false));
  ASSERT_TRUE(correct_response);

  // Ensure that the port ID assigned to the table is not 0 and matches the
  // port id retrieved from getsockname() syscall.
  uint32_t assigned_port_id =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkPortID(fd.get()));
  ASSERT_NE(expected_port_id, 0);
  ASSERT_NE(assigned_port_id, 0);
  ASSERT_EQ(expected_port_id, assigned_port_id);
}

TEST_F(NetlinkNetfilterTest, ErrAddRuleWithMissingTableName) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_rule_request_buffer =
      NlReq("newrule req ack inet").Seq(kSeq + 2).Build();

  AddDefaultTable({.fd = fd, .seq = kSeq});
  AddDefaultBaseChain({.fd = fd, .seq = kSeq + 1});
  ASSERT_THAT(
      NetlinkRequestAckOrError(fd, kSeq + 2, add_rule_request_buffer.data(),
                               add_rule_request_buffer.size()),
      PosixErrorIs(EINVAL, _));
}

TEST_F(NetlinkNetfilterTest, ErrAddRuleWithUnknownTableName) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_rule_request_buffer =
      NlReq("newrule req ack inet")
          .Seq(kSeq + 2)
          .StrAttr(NFTA_RULE_TABLE, "unknown_table_name")
          .Build();

  AddDefaultTable({.fd = fd, .seq = kSeq});
  AddDefaultBaseChain({.fd = fd, .seq = kSeq + 1});
  ASSERT_THAT(
      NetlinkRequestAckOrError(fd, kSeq + 2, add_rule_request_buffer.data(),
                               add_rule_request_buffer.size()),
      PosixErrorIs(ENOENT, _));
}

TEST_F(NetlinkNetfilterTest, ErrAddRuleNoChainSpecified) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_rule_request_buffer =
      NlReq("newrule req ack inet")
          .Seq(kSeq + 2)
          .StrAttr(NFTA_RULE_TABLE, DEFAULT_TABLE_NAME)
          .Build();

  AddDefaultTable({.fd = fd, .seq = kSeq});
  AddDefaultBaseChain({.fd = fd, .seq = kSeq + 1});
  ASSERT_THAT(
      NetlinkRequestAckOrError(fd, kSeq + 2, add_rule_request_buffer.data(),
                               add_rule_request_buffer.size()),
      PosixErrorIs(EINVAL, _));
}

TEST_F(NetlinkNetfilterTest,
       ErrAddRuleNoHandleOrPositionSpecifiedAndCreateReplaceFlagNotSet) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_rule_request_buffer =
      NlReq("newrule req ack inet")
          .Seq(kSeq + 2)
          .StrAttr(NFTA_RULE_TABLE, DEFAULT_TABLE_NAME)
          .StrAttr(NFTA_RULE_CHAIN, DEFAULT_CHAIN_NAME)
          .Build();

  AddDefaultTable({.fd = fd, .seq = kSeq});
  AddDefaultBaseChain({.fd = fd, .seq = kSeq + 1});
  ASSERT_THAT(
      NetlinkRequestAckOrError(fd, kSeq + 2, add_rule_request_buffer.data(),
                               add_rule_request_buffer.size()),
      PosixErrorIs(EINVAL, _));
}

TEST_F(NetlinkNetfilterTest, ErrAddRuleNoHandleOrPositionSpecified) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  std::vector<char> add_rule_request_buffer =
      NlReq("newrule req ack inet")
          .Seq(kSeq + 2)
          .StrAttr(NFTA_RULE_TABLE, DEFAULT_TABLE_NAME)
          .StrAttr(NFTA_RULE_CHAIN, DEFAULT_CHAIN_NAME)
          .Build();

  AddDefaultTable({.fd = fd, .seq = kSeq});
  AddDefaultBaseChain({.fd = fd, .seq = kSeq + 1});
  ASSERT_THAT(
      NetlinkRequestAckOrError(fd, kSeq + 2, add_rule_request_buffer.data(),
                               add_rule_request_buffer.size()),
      PosixErrorIs(EINVAL, _));
}

TEST_F(NetlinkNetfilterTest, ErrAddRuleInvalidPositionSpecified) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  uint64_t invalid_position = 10;
  std::vector<char> add_rule_request_buffer =
      NlReq("newrule req ack create inet")
          .Seq(kSeq + 2)
          .StrAttr(NFTA_RULE_TABLE, DEFAULT_TABLE_NAME)
          .StrAttr(NFTA_RULE_CHAIN, DEFAULT_CHAIN_NAME)
          .U64Attr(NFTA_RULE_POSITION, &invalid_position)
          .Build();

  AddDefaultTable({.fd = fd, .seq = kSeq});
  AddDefaultBaseChain({.fd = fd, .seq = kSeq + 1});
  ASSERT_THAT(
      NetlinkRequestAckOrError(fd, kSeq + 2, add_rule_request_buffer.data(),
                               add_rule_request_buffer.size()),
      PosixErrorIs(ENOENT, _));
}

TEST_F(NetlinkNetfilterTest, ErrAddRuleInvalidHandleSpecified) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  uint64_t invalid_handle = 10;
  std::vector<char> add_rule_request_buffer =
      NlReq("newrule req ack inet")
          .Seq(kSeq + 2)
          .StrAttr(NFTA_RULE_TABLE, DEFAULT_TABLE_NAME)
          .StrAttr(NFTA_RULE_CHAIN, DEFAULT_CHAIN_NAME)
          .U64Attr(NFTA_RULE_HANDLE, &invalid_handle)
          .Build();

  AddDefaultTable({.fd = fd, .seq = kSeq});
  AddDefaultBaseChain({.fd = fd, .seq = kSeq + 1});
  ASSERT_THAT(
      NetlinkRequestAckOrError(fd, kSeq + 2, add_rule_request_buffer.data(),
                               add_rule_request_buffer.size()),
      PosixErrorIs(ENOENT, _));
}

TEST_F(NetlinkNetfilterTest, AddEmptyRule) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  uint8_t expected_udata[] = {0, 1, 2, 3, 4};
  std::vector<char> add_rule_request_buffer =
      NlReq("newrule req ack create inet")
          .Seq(kSeq + 2)
          .StrAttr(NFTA_RULE_TABLE, DEFAULT_TABLE_NAME)
          .StrAttr(NFTA_RULE_CHAIN, DEFAULT_CHAIN_NAME)
          .RawAttr(NFTA_RULE_USERDATA, expected_udata, sizeof(expected_udata))
          .Build();

  AddDefaultTable({.fd = fd, .seq = kSeq});
  AddDefaultBaseChain({.fd = fd, .seq = kSeq + 1});
  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq + 2,
                                           add_rule_request_buffer.data(),
                                           add_rule_request_buffer.size()));
}

TEST_F(NetlinkNetfilterTest, ErrRuleExpressionWrongType) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  uint8_t udata[] = {0, 1, 2, 3, 4};
  std::vector<char> rule_expr_data = NlImmExpr::DefaultAcceptAll();
  std::vector<char> list_expr_data = NlListAttr().Add(rule_expr_data).Build();
  struct nlattr* attr = reinterpret_cast<struct nlattr*>(list_expr_data.data());
  attr->nla_type = NFTA_LIST_UNSPEC;

  std::vector<char> add_rule_request_buffer =
      NlReq("newrule req ack create inet")
          .Seq(kSeq + 2)
          .StrAttr(NFTA_RULE_TABLE, DEFAULT_TABLE_NAME)
          .StrAttr(NFTA_RULE_CHAIN, DEFAULT_CHAIN_NAME)
          .RawAttr(NFTA_RULE_USERDATA, udata, sizeof(udata))
          .RawAttr(NFTA_RULE_EXPRESSIONS, list_expr_data.data(),
                   list_expr_data.size())
          .Build();

  AddDefaultTable({.fd = fd, .seq = kSeq});
  AddDefaultBaseChain({.fd = fd, .seq = kSeq + 1});
  ASSERT_THAT(
      NetlinkRequestAckOrError(fd, kSeq + 2, add_rule_request_buffer.data(),
                               add_rule_request_buffer.size()),
      PosixErrorIs(EINVAL, _));
}

TEST_F(NetlinkNetfilterTest, ErrRuleTooManyExpressions) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  uint8_t udata[] = {0, 1, 2, 3, 4};
  std::vector<char> rule_expr_data = NlImmExpr::DefaultAcceptAll();
  std::vector<char> list_expr_data = NlListAttr::BuildWithMaxAttrs();

  std::vector<char> add_rule_request_buffer =
      NlReq("newrule req ack create inet")
          .Seq(kSeq + 2)
          .StrAttr(NFTA_RULE_TABLE, DEFAULT_TABLE_NAME)
          .StrAttr(NFTA_RULE_CHAIN, DEFAULT_CHAIN_NAME)
          .RawAttr(NFTA_RULE_USERDATA, udata, sizeof(udata))
          .RawAttr(NFTA_RULE_EXPRESSIONS, list_expr_data.data(),
                   list_expr_data.size())
          .Build();

  AddDefaultTable({.fd = fd, .seq = kSeq});
  AddDefaultBaseChain({.fd = fd, .seq = kSeq + 1});
  ASSERT_THAT(
      NetlinkRequestAckOrError(fd, kSeq + 2, add_rule_request_buffer.data(),
                               add_rule_request_buffer.size()),
      PosixErrorIs(EINVAL, _));
}

TEST_F(NetlinkNetfilterTest, ErrImmRuleNoDestinationRegisterSpecified) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  uint8_t udata[] = {0, 1, 2};
  std::vector<char> rule_data = {0, 1, 2};
  std::vector<char> immediate_data =
      NlNestedAttr()
          .RawAttr(NFTA_DATA_VALUE, rule_data.data(), rule_data.size())
          .Build();
  std::vector<char> immediate_attrs =
      NlNestedAttr()
          .RawAttr(NFTA_IMMEDIATE_DATA, immediate_data.data(),
                   immediate_data.size())
          .Build();
  std::vector<char> rule_expr_data =
      NlNestedAttr()
          .StrAttr(NFTA_EXPR_NAME, "immediate")
          .RawAttr(NFTA_EXPR_DATA, immediate_attrs.data(),
                   immediate_attrs.size())
          .Build();
  std::vector<char> list_expr_data = NlListAttr().Add(rule_expr_data).Build();

  std::vector<char> add_rule_request_buffer =
      NlReq("newrule req ack create inet")
          .Seq(kSeq + 2)
          .StrAttr(NFTA_RULE_TABLE, DEFAULT_TABLE_NAME)
          .StrAttr(NFTA_RULE_CHAIN, DEFAULT_CHAIN_NAME)
          .RawAttr(NFTA_RULE_USERDATA, udata, sizeof(udata))
          .RawAttr(NFTA_RULE_EXPRESSIONS, list_expr_data.data(),
                   list_expr_data.size())
          .Build();

  AddDefaultTable({.fd = fd, .seq = kSeq});
  AddDefaultBaseChain({.fd = fd, .seq = kSeq + 1});
  ASSERT_THAT(
      NetlinkRequestAckOrError(fd, kSeq + 2, add_rule_request_buffer.data(),
                               add_rule_request_buffer.size()),
      PosixErrorIs(EINVAL, _));
}

TEST_F(NetlinkNetfilterTest, ErrImmRuleNoDataSpecified) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  uint8_t udata[] = {0, 1, 2};
  uint32_t dreg = NFT_REG_VERDICT;
  std::vector<char> rule_data = {0, 1, 2};
  std::vector<char> immediate_attrs =
      NlNestedAttr().U32Attr(NFTA_IMMEDIATE_DREG, &dreg).Build();
  std::vector<char> rule_expr_data =
      NlNestedAttr()
          .StrAttr(NFTA_EXPR_NAME, "immediate")
          .RawAttr(NFTA_EXPR_DATA, immediate_attrs.data(),
                   immediate_attrs.size())
          .Build();
  std::vector<char> list_expr_data = NlListAttr().Add(rule_expr_data).Build();

  std::vector<char> add_rule_request_buffer =
      NlReq("newrule req ack create inet")
          .Seq(kSeq + 2)
          .StrAttr(NFTA_RULE_TABLE, DEFAULT_TABLE_NAME)
          .StrAttr(NFTA_RULE_CHAIN, DEFAULT_CHAIN_NAME)
          .RawAttr(NFTA_RULE_USERDATA, udata, sizeof(udata))
          .RawAttr(NFTA_RULE_EXPRESSIONS, list_expr_data.data(),
                   list_expr_data.size())
          .Build();

  AddDefaultTable({.fd = fd, .seq = kSeq});
  AddDefaultBaseChain({.fd = fd, .seq = kSeq + 1});
  ASSERT_THAT(
      NetlinkRequestAckOrError(fd, kSeq + 2, add_rule_request_buffer.data(),
                               add_rule_request_buffer.size()),
      PosixErrorIs(EINVAL, _));
}

TEST_F(NetlinkNetfilterTest, ErrValueDataWithVerdictRegister) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  uint8_t udata[] = {0, 1, 2};
  std::vector<char> rule_expr_data =
      NlImmExpr().Dreg(NFT_REG_VERDICT).VerdictCode(NF_ACCEPT).ValueBuild();
  std::vector<char> list_expr_data = NlListAttr().Add(rule_expr_data).Build();

  std::vector<char> add_rule_request_buffer =
      NlReq("newrule req ack create inet")
          .Seq(kSeq + 2)
          .StrAttr(NFTA_RULE_TABLE, DEFAULT_TABLE_NAME)
          .StrAttr(NFTA_RULE_CHAIN, DEFAULT_CHAIN_NAME)
          .RawAttr(NFTA_RULE_USERDATA, udata, sizeof(udata))
          .RawAttr(NFTA_RULE_EXPRESSIONS, list_expr_data.data(),
                   list_expr_data.size())
          .Build();

  AddDefaultTable({.fd = fd, .seq = kSeq});
  AddDefaultBaseChain({.fd = fd, .seq = kSeq + 1});
  ASSERT_THAT(
      NetlinkRequestAckOrError(fd, kSeq + 2, add_rule_request_buffer.data(),
                               add_rule_request_buffer.size()),
      PosixErrorIs(EINVAL, _));
}

TEST_F(NetlinkNetfilterTest, ErrVerdictDataWithNonVerdictRegister) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  uint8_t udata[] = {0, 1, 2};
  std::vector<char> rule_expr_data =
      NlImmExpr().Dreg(NFT_REG_1).VerdictCode(NF_ACCEPT).VerdictBuild();
  std::vector<char> list_expr_data = NlListAttr().Add(rule_expr_data).Build();

  std::vector<char> add_rule_request_buffer =
      NlReq("newrule req ack create inet")
          .Seq(kSeq + 2)
          .StrAttr(NFTA_RULE_TABLE, DEFAULT_TABLE_NAME)
          .StrAttr(NFTA_RULE_CHAIN, DEFAULT_CHAIN_NAME)
          .RawAttr(NFTA_RULE_USERDATA, udata, sizeof(udata))
          .RawAttr(NFTA_RULE_EXPRESSIONS, list_expr_data.data(),
                   list_expr_data.size())
          .Build();

  AddDefaultTable({.fd = fd, .seq = kSeq});
  AddDefaultBaseChain({.fd = fd, .seq = kSeq + 1});
  ASSERT_THAT(
      NetlinkRequestAckOrError(fd, kSeq + 2, add_rule_request_buffer.data(),
                               add_rule_request_buffer.size()),
      PosixErrorIs(EINVAL, _));
}

TEST_F(NetlinkNetfilterTest, ErrExpressionDataMalformed) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  uint8_t udata[] = {0, 1, 2};
  uint32_t dreg = NFT_REG_1;
  std::vector<char> rule_data = {0, 1, 2};
  std::vector<char> immediate_data =
      NlNestedAttr().RawAttr(20, rule_data.data(), rule_data.size()).Build();
  std::vector<char> immediate_attrs =
      NlNestedAttr()
          .U32Attr(NFTA_IMMEDIATE_DREG, &dreg)
          .RawAttr(NFTA_IMMEDIATE_DATA, immediate_data.data(),
                   immediate_data.size())
          .Build();
  std::vector<char> rule_expr_data =
      NlNestedAttr()
          .StrAttr(NFTA_EXPR_NAME, "immediate")
          .RawAttr(NFTA_EXPR_DATA, immediate_attrs.data(),
                   immediate_attrs.size())
          .Build();
  std::vector<char> list_expr_data = NlListAttr().Add(rule_expr_data).Build();

  std::vector<char> add_rule_request_buffer =
      NlReq("newrule req ack create inet")
          .Seq(kSeq + 2)
          .StrAttr(NFTA_RULE_TABLE, DEFAULT_TABLE_NAME)
          .StrAttr(NFTA_RULE_CHAIN, DEFAULT_CHAIN_NAME)
          .RawAttr(NFTA_RULE_USERDATA, udata, sizeof(udata))
          .RawAttr(NFTA_RULE_EXPRESSIONS, list_expr_data.data(),
                   list_expr_data.size())
          .Build();

  AddDefaultTable({.fd = fd, .seq = kSeq});
  AddDefaultBaseChain({.fd = fd, .seq = kSeq + 1});
  ASSERT_THAT(
      NetlinkRequestAckOrError(fd, kSeq + 2, add_rule_request_buffer.data(),
                               add_rule_request_buffer.size()),
      PosixErrorIs(EINVAL, _));
}

TEST_F(NetlinkNetfilterTest, ErrImmInvalidDreg) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  uint8_t udata[] = {0, 1, 2};
  uint32_t dreg = 1000;
  std::vector<char> rule_data = {0, 1, 2};
  std::vector<char> immediate_data =
      NlNestedAttr()
          .RawAttr(NFTA_DATA_VALUE, rule_data.data(), rule_data.size())
          .Build();
  std::vector<char> immediate_attrs =
      NlNestedAttr()
          .U32Attr(NFTA_IMMEDIATE_DREG, &dreg)
          .RawAttr(NFTA_IMMEDIATE_DATA, immediate_data.data(),
                   immediate_data.size())
          .Build();
  std::vector<char> rule_expr_data =
      NlNestedAttr()
          .StrAttr(NFTA_EXPR_NAME, "immediate")
          .RawAttr(NFTA_EXPR_DATA, immediate_attrs.data(),
                   immediate_attrs.size())
          .Build();
  std::vector<char> list_expr_data = NlListAttr().Add(rule_expr_data).Build();

  std::vector<char> add_rule_request_buffer =
      NlReq("newrule req ack create inet")
          .Seq(kSeq + 2)
          .StrAttr(NFTA_RULE_TABLE, DEFAULT_TABLE_NAME)
          .StrAttr(NFTA_RULE_CHAIN, DEFAULT_CHAIN_NAME)
          .RawAttr(NFTA_RULE_USERDATA, udata, sizeof(udata))
          .RawAttr(NFTA_RULE_EXPRESSIONS, list_expr_data.data(),
                   list_expr_data.size())
          .Build();

  AddDefaultTable({.fd = fd, .seq = kSeq});
  AddDefaultBaseChain({.fd = fd, .seq = kSeq + 1});
  ASSERT_THAT(
      NetlinkRequestAckOrError(fd, kSeq + 2, add_rule_request_buffer.data(),
                               add_rule_request_buffer.size()),
      PosixErrorIs(ERANGE, _));
}

TEST_F(NetlinkNetfilterTest, AddAcceptAllRule) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  uint8_t udata[] = {0, 1, 2};
  std::vector<char> rule_expr_data = NlImmExpr::DefaultAcceptAll();
  std::vector<char> list_expr_data = NlListAttr().Add(rule_expr_data).Build();

  std::vector<char> add_rule_request_buffer =
      NlReq("newrule req ack create inet")
          .Seq(kSeq + 2)
          .StrAttr(NFTA_RULE_TABLE, DEFAULT_TABLE_NAME)
          .StrAttr(NFTA_RULE_CHAIN, DEFAULT_CHAIN_NAME)
          .RawAttr(NFTA_RULE_USERDATA, udata, sizeof(udata))
          .RawAttr(NFTA_RULE_EXPRESSIONS, list_expr_data.data(),
                   list_expr_data.size())
          .Build();

  AddDefaultTable({.fd = fd, .seq = kSeq});
  AddDefaultBaseChain({.fd = fd, .seq = kSeq + 1});
  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq + 2,
                                           add_rule_request_buffer.data(),
                                           add_rule_request_buffer.size()));
}

TEST_F(NetlinkNetfilterTest, AddDropAllRule) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  uint8_t udata[] = {0, 1, 2};
  std::vector<char> rule_expr_data = NlImmExpr::DefaultDropAll();
  std::vector<char> list_expr_data = NlListAttr().Add(rule_expr_data).Build();
  std::vector<char> add_rule_request_buffer =
      NlReq("newrule req ack create inet")
          .Seq(kSeq + 2)
          .StrAttr(NFTA_RULE_TABLE, DEFAULT_TABLE_NAME)
          .StrAttr(NFTA_RULE_CHAIN, DEFAULT_CHAIN_NAME)
          .RawAttr(NFTA_RULE_USERDATA, udata, sizeof(udata))
          .RawAttr(NFTA_RULE_EXPRESSIONS, list_expr_data.data(),
                   list_expr_data.size())
          .Build();

  AddDefaultTable({.fd = fd, .seq = kSeq});
  AddDefaultBaseChain({.fd = fd, .seq = kSeq + 1});
  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq + 2,
                                           add_rule_request_buffer.data(),
                                           add_rule_request_buffer.size()));
}

TEST_F(NetlinkNetfilterTest, AddRuleWithImmDataValue) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  uint8_t udata[] = {0, 1, 2, 3, 4};
  uint32_t dreg = NFT_REG_1;
  std::vector<char> rule_data = {0, 1, 2};
  std::vector<char> rule_expr_data =
      NlImmExpr().Dreg(dreg).Value(rule_data).ValueBuild();
  std::vector<char> list_expr_data = NlListAttr().Add(rule_expr_data).Build();

  std::vector<char> add_rule_request_buffer =
      NlReq("newrule req ack create inet")
          .Seq(kSeq + 2)
          .StrAttr(NFTA_RULE_TABLE, DEFAULT_TABLE_NAME)
          .StrAttr(NFTA_RULE_CHAIN, DEFAULT_CHAIN_NAME)
          .RawAttr(NFTA_RULE_USERDATA, udata, sizeof(udata))
          .RawAttr(NFTA_RULE_EXPRESSIONS, list_expr_data.data(),
                   list_expr_data.size())
          .Build();

  AddDefaultTable({.fd = fd, .seq = kSeq});
  AddDefaultBaseChain({.fd = fd, .seq = kSeq + 1});
  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq + 2,
                                           add_rule_request_buffer.data(),
                                           add_rule_request_buffer.size()));
}

TEST_F(NetlinkNetfilterTest, AddRuleToEndOfRuleList) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  uint8_t udata[] = {0, 1, 2};
  std::vector<char> rule_expr_data = NlImmExpr::DefaultAcceptAll();
  std::vector<char> list_expr_data = NlListAttr().Add(rule_expr_data).Build();

  std::vector<char> add_rule_request_buffer =
      NlReq("newrule req ack create inet")
          .Seq(kSeq + 2)
          .StrAttr(NFTA_RULE_TABLE, DEFAULT_TABLE_NAME)
          .StrAttr(NFTA_RULE_CHAIN, DEFAULT_CHAIN_NAME)
          .RawAttr(NFTA_RULE_USERDATA, udata, sizeof(udata))
          .RawAttr(NFTA_RULE_EXPRESSIONS, list_expr_data.data(),
                   list_expr_data.size())
          .Build();

  std::vector<char> add_rule_request_buffer_2 =
      NlReq("newrule req ack create append inet")
          .Seq(kSeq + 3)
          .StrAttr(NFTA_RULE_TABLE, DEFAULT_TABLE_NAME)
          .StrAttr(NFTA_RULE_CHAIN, DEFAULT_CHAIN_NAME)
          .RawAttr(NFTA_RULE_USERDATA, udata, sizeof(udata))
          .RawAttr(NFTA_RULE_EXPRESSIONS, list_expr_data.data(),
                   list_expr_data.size())
          .Build();

  AddDefaultTable({.fd = fd, .seq = kSeq});
  AddDefaultBaseChain({.fd = fd, .seq = kSeq + 1});
  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq + 2,
                                           add_rule_request_buffer.data(),
                                           add_rule_request_buffer.size()));
  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(fd, kSeq + 3,
                                           add_rule_request_buffer_2.data(),
                                           add_rule_request_buffer_2.size()));
}

TEST_F(NetlinkNetfilterTest, AddDropRuleBeforeAcceptRule) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  uint8_t udata[] = {0, 1, 2};
  std::vector<char> rule_expr_data = NlImmExpr::DefaultAcceptAll();
  std::vector<char> list_expr_data = NlListAttr().Add(rule_expr_data).Build();

  std::vector<char> add_rule_accept_request_buffer =
      NlReq("newrule req ack create inet")
          .Seq(kSeq + 2)
          .StrAttr(NFTA_RULE_TABLE, DEFAULT_TABLE_NAME)
          .StrAttr(NFTA_RULE_CHAIN, DEFAULT_CHAIN_NAME)
          .RawAttr(NFTA_RULE_USERDATA, udata, sizeof(udata))
          .RawAttr(NFTA_RULE_EXPRESSIONS, list_expr_data.data(),
                   list_expr_data.size())
          .Build();

  // TODO - b/421437663: Change to use GET_RULE to retrieve the rule handle
  // dynamically.
  uint64_t rule_handle = 2;
  std::vector<char> rule_expr_drop = NlImmExpr::DefaultDropAll();
  std::vector<char> list_expr_data_2 = NlListAttr().Add(rule_expr_drop).Build();
  std::vector<char> add_rule_drop_request_buffer =
      NlReq("newrule req ack create append inet")
          .Seq(kSeq + 3)
          .StrAttr(NFTA_RULE_TABLE, DEFAULT_TABLE_NAME)
          .StrAttr(NFTA_RULE_CHAIN, DEFAULT_CHAIN_NAME)
          .U64Attr(NFTA_RULE_POSITION, &rule_handle)
          .RawAttr(NFTA_RULE_USERDATA, udata, sizeof(udata))
          .RawAttr(NFTA_RULE_EXPRESSIONS, list_expr_data_2.data(),
                   list_expr_data_2.size())
          .Build();

  AddDefaultTable({.fd = fd, .seq = kSeq});
  AddDefaultBaseChain({.fd = fd, .seq = kSeq + 1});
  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(
      fd, kSeq + 2, add_rule_accept_request_buffer.data(),
      add_rule_accept_request_buffer.size()));
  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(
      fd, kSeq + 3, add_rule_drop_request_buffer.data(),
      add_rule_drop_request_buffer.size()));
}

TEST_F(NetlinkNetfilterTest, AddDropRuleAfterAcceptRule) {
  SKIP_IF(!ASSERT_NO_ERRNO_AND_VALUE(HaveCapability(CAP_NET_RAW)));
  FileDescriptor fd =
      ASSERT_NO_ERRNO_AND_VALUE(NetlinkBoundSocket(NETLINK_NETFILTER));

  uint8_t udata[] = {0, 1, 2};
  std::vector<char> rule_expr_data = NlImmExpr::DefaultAcceptAll();
  std::vector<char> list_expr_data = NlListAttr().Add(rule_expr_data).Build();

  std::vector<char> add_rule_accept_request_buffer =
      NlReq("newrule req ack create append inet")
          .Seq(kSeq + 2)
          .StrAttr(NFTA_RULE_TABLE, DEFAULT_TABLE_NAME)
          .StrAttr(NFTA_RULE_CHAIN, DEFAULT_CHAIN_NAME)
          .RawAttr(NFTA_RULE_USERDATA, udata, sizeof(udata))
          .RawAttr(NFTA_RULE_EXPRESSIONS, list_expr_data.data(),
                   list_expr_data.size())
          .Build();

  // TODO - b/421437663: Change to use GET_RULE to retrieve the rule handle
  // dynamically.
  uint64_t rule_handle = 2;
  std::vector<char> rule_expr_drop = NlImmExpr::DefaultDropAll();
  std::vector<char> list_expr_data_2 = NlListAttr().Add(rule_expr_drop).Build();
  std::vector<char> add_rule_drop_request_buffer =
      NlReq("newrule req ack create append inet")
          .Seq(kSeq + 3)
          .StrAttr(NFTA_RULE_TABLE, DEFAULT_TABLE_NAME)
          .StrAttr(NFTA_RULE_CHAIN, DEFAULT_CHAIN_NAME)
          .U64Attr(NFTA_RULE_POSITION, &rule_handle)
          .RawAttr(NFTA_RULE_USERDATA, udata, sizeof(udata))
          .RawAttr(NFTA_RULE_EXPRESSIONS, list_expr_data_2.data(),
                   list_expr_data_2.size())
          .Build();

  AddDefaultTable({.fd = fd, .seq = kSeq});
  AddDefaultBaseChain({.fd = fd, .seq = kSeq + 1});
  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(
      fd, kSeq + 2, add_rule_accept_request_buffer.data(),
      add_rule_accept_request_buffer.size()));
  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(
      fd, kSeq + 3, add_rule_drop_request_buffer.data(),
      add_rule_drop_request_buffer.size()));
}

}  // namespace

}  // namespace testing
}  // namespace gvisor
