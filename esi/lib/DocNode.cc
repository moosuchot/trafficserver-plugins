#include "DocNode.h"
#include "Utils.h"

using std::string;
using namespace EsiLib;

const DocNode::TYPE DocNode::TYPE_UNKNOWN = 0;
const DocNode::TYPE DocNode::TYPE_PRE = 1;
const DocNode::TYPE DocNode::TYPE_INCLUDE = 2;
const DocNode::TYPE DocNode::TYPE_COMMENT = 3;
const DocNode::TYPE DocNode::TYPE_REMOVE = 4;
const DocNode::TYPE DocNode::TYPE_VARS = 5;
const DocNode::TYPE DocNode::TYPE_CHOOSE = 6;
const DocNode::TYPE DocNode::TYPE_WHEN = 7;
const DocNode::TYPE DocNode::TYPE_OTHERWISE = 8;
const DocNode::TYPE DocNode::TYPE_TRY = 9;
const DocNode::TYPE DocNode::TYPE_ATTEMPT = 10;
const DocNode::TYPE DocNode::TYPE_EXCEPT = 11;
const DocNode::TYPE DocNode::TYPE_HTML_COMMENT = 12;
const DocNode::TYPE DocNode::TYPE_SPECIAL_INCLUDE = 13;

const char *DocNode::type_names_[] = { "UNKNOWN",
                                       "PRE",
                                       "INCLUDE",
                                       "COMMENT",
                                       "REMOVE",
                                       "VARS",
                                       "CHOOSE",
                                       "WHEN",
                                       "OTHERWISE",
                                       "TRY",
                                       "ATTEMPT",
                                       "EXCEPT",
                                       "HTML_COMMENT",
                                       "SPECIAL_INCLUDE"
};

const char DocNode::VERSION = 1;

// helper functions 

inline void
packString(const char *str, int32_t str_len, string &buffer) {
  buffer.append(reinterpret_cast<const char *>(&str_len), sizeof(str_len));
  if (str_len) {
    buffer.append(str, str_len);
  }
}

inline void
unpackString(const char *&packed_data, const char *&item, int32_t &item_len) {
  item_len = *(reinterpret_cast<const int32_t *>(packed_data));
  packed_data += sizeof(int32_t);
  item = item_len ? packed_data : 0;
  packed_data += item_len;
}

template<typename T> inline void
unpackItem(const char *&packed_data, T &item) {
  item = *(reinterpret_cast<const T *>(packed_data));
  packed_data += sizeof(T);
}

void
DocNode::pack(string &buffer) const {
  int32_t orig_buf_size = buffer.size();
  buffer += VERSION;
  buffer.append(sizeof(int32_t), ' '); // reserve space for length
  buffer.append(reinterpret_cast<const char *>(&type), sizeof(type));
  packString(data, data_len, buffer);
  int32_t n_elements = attr_list.size();
  buffer.append(reinterpret_cast<const char *>(&n_elements), sizeof(n_elements));
  for (AttributeList::const_iterator iter = attr_list.begin(); iter != attr_list.end(); ++iter) {
    packString(iter->name, iter->name_len, buffer);
    packString(iter->value, iter->value_len, buffer);
  }
  child_nodes.packToBuffer(buffer);
  *(reinterpret_cast<int32_t *>(&buffer[orig_buf_size + 1])) = buffer.size() - orig_buf_size;
}

bool
DocNode::unpack(const char *packed_data, int packed_data_len, int &node_len) {
  const char *packed_data_start = packed_data;

  if (!packed_data || (packed_data_len < static_cast<int>((sizeof(char) + sizeof(int32_t))))) {
    Utils::ERROR_LOG("[%s] Invalid arguments (%p, %d)", __FUNCTION__, packed_data, packed_data_len);
    return false;
  }
  if (*packed_data != VERSION) {
    Utils::ERROR_LOG("[%s] Version %d not in supported set (%d)",
                     __FUNCTION__, static_cast<int>(*packed_data), static_cast<int>(VERSION));
    return false;
  }
  ++packed_data;

  int32_t node_size;
  unpackItem(packed_data, node_size);
  if (node_size > packed_data_len) {
    Utils::ERROR_LOG("[%s] Data size (%d) not sufficient to hold node of size %d",
                     __FUNCTION__, packed_data_len, node_size);
    return false;
  }
  node_len = node_size;

  unpackItem(packed_data, type);

  unpackString(packed_data, data, data_len);

  int32_t n_elements;
  unpackItem(packed_data, n_elements);
  Attribute attr;
  attr_list.clear();
  for (int i = 0; i < n_elements; ++i) {
    unpackString(packed_data, attr.name, attr.name_len);
    unpackString(packed_data, attr.value, attr.value_len);
    attr_list.push_back(attr);
  }

  if (!child_nodes.unpack(packed_data, packed_data_len - (packed_data - packed_data_start))) {
    Utils::ERROR_LOG("[%s] Could not unpack child nodes", __FUNCTION__);
    return false;
  }
  return true;
}

void
DocNodeList::packToBuffer(string &buffer) const {
  int32_t n_elements = size();
  buffer.append(reinterpret_cast<const char *>(&n_elements), sizeof(n_elements));
  for (DocNodeList::const_iterator iter = begin(); iter != end(); ++iter) {
    iter->pack(buffer);
  }
}

bool
DocNodeList::unpack(const char *data, int data_len) {
  if (!data || (data_len < static_cast<int>(sizeof(int32_t)))) {
    Utils::ERROR_LOG("[%s] Invalid arguments", __FUNCTION__);
    return false;
  }
  const char *data_start = data;
  int32_t n_elements;
  unpackItem(data, n_elements);
  clear();
  int data_offset = data - data_start, node_size;
  DocNode node;
  for (int i = 0; i < n_elements; ++i) {
    if (!node.unpack(data_start + data_offset, data_len - data_offset, node_size)) {
      Utils::ERROR_LOG("[%s] Could not unpack node", __FUNCTION__);
      return false;
    }
    data_offset += node_size;
    push_back(node);
  }
  return true;
}
