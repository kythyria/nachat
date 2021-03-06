#include "Room.hpp"

#include <unordered_set>
#include <memory>
#include <algorithm>

#include <QtNetwork>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QDebug>

#include "proto.hpp"
#include "Session.hpp"
#include "utils.hpp"
#include "parse.hpp"

namespace matrix {

RoomState::RoomState(const QJsonObject &info, lmdb::txn &txn, lmdb::dbi &member_db)
    : name_(info["name"].toString()),
      canonical_alias_(info["canonical_alias"].toString()),
      topic_(info["topic"].toString()),
      avatar_(info["avatar"].toString()) {
  const auto &as = info["aliases"].toArray();
  aliases_.reserve(as.size());
  std::transform(as.begin(), as.end(), std::back_inserter(aliases_),
                 [](const QJsonValue &v) {
                   return v.toString();
                 });

  lmdb::val id_val;
  lmdb::val state;
  auto cursor = lmdb::cursor::open(txn, member_db);
  while(cursor.get(id_val, state, MDB_NEXT)) {
    const auto id_str = QString::fromUtf8(id_val.data(), id_val.size());
    auto &member = members_by_id_.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(id_str),
      std::forward_as_tuple(id_str, QJsonDocument::fromBinaryData(QByteArray(state.data(), state.size())).object())).first->second;
    record_displayname(member.id(), member.display_name(), nullptr);
  }
}

QJsonObject RoomState::to_json() const {
  QJsonObject o;
  if(!name_.isNull()) o["name"] = name_;
  if(!canonical_alias_.isNull()) o["canonical_alias"] = canonical_alias_;
  if(!topic_.isNull()) o["topic"] = topic_;
  if(!avatar_.isEmpty()) o["avatar"] = avatar_.toString(QUrl::FullyEncoded);

  QJsonArray aa;
  for(const auto &x : aliases_) {
    aa.push_back(x);
  }
  o["aliases"] = std::move(aa);

  return o;
}

QString RoomState::pretty_name(const QString &own_id) const {
  if(!name_.isEmpty()) return name_;
  if(!canonical_alias_.isEmpty()) return canonical_alias_;
  if(!aliases_.empty()) return aliases_[0];  // Non-standard, but matches vector-web
  // FIXME: Maintain earliest two IDs as state!
  auto ms = members();
  ms.erase(std::remove_if(ms.begin(), ms.end(), [&](const Member *m){ return m->id() == own_id; }), ms.end());
  if(ms.size() > 1) {
    std::partial_sort(ms.begin(), ms.begin() + 2, ms.end(),
                      [](const Member *a, const Member *b) {
                        return a->id() < b->id();
                      });
  }
  switch(ms.size()) {
  case 0: return QObject::tr("Empty room");
  case 1: return ms[0]->pretty_name();
  case 2: return QObject::tr("%1 and %2").arg(member_name(*ms[0])).arg(member_name(*ms[1]));
  default: return QObject::tr("%1 and %2 others").arg(member_name(*ms[0])).arg(ms.size() - 1);
  }
}

QString RoomState::member_disambiguation(const Member &member) const {
  if(member.display_name().isEmpty()) {
    if(members_by_displayname_.find(member.id()) != members_by_displayname_.end()) {
      return member.id();
    } else {
      return "";
    }
  } else if(members_named(member.display_name()).size() > 1 || member_from_id(member.display_name())) {
    return member.id();
  } else {
    return "";
  }
}

QString RoomState::member_name(const Member &member) const {
  auto result = member.pretty_name();
  auto disambig = member_disambiguation(member);
  if(disambig.isEmpty()) return result;
  return result % " (" % disambig % ")";
}

std::vector<UserID> &RoomState::members_named(QString displayname) {
  return members_by_displayname_.at(displayname.normalized(QString::NormalizationForm_C));
}
const std::vector<UserID> &RoomState::members_named(QString displayname) const {
  return members_by_displayname_.at(displayname.normalized(QString::NormalizationForm_C));
}

std::vector<const Member *> RoomState::members() const {
  std::vector<const Member *> result;
  result.reserve(members_by_id_.size());
  std::transform(members_by_id_.begin(), members_by_id_.end(), std::back_inserter(result),
                 [](auto &x) { return &x.second; });
  return result;
}

void RoomState::forget_displayname(const UserID &id, const QString &old_name_in, Room *room) {
  if(old_name_in.isEmpty()) return;

  const QString old_name = old_name_in.normalized(QString::NormalizationForm_C);
  auto &vec = members_by_displayname_.at(old_name);
  QString other_disambiguation;
  const Member *other_member = nullptr;
  const bool existing_displayname = vec.size() == 2;
  const Member *const existing_mxid = member_from_id(old_name);
  if(room && (!existing_displayname || !existing_mxid)) {
    if(existing_displayname) other_member = &members_by_id_.at(vec[0] == id ? vec[1] : vec[0]);
    if(existing_mxid) other_member = existing_mxid;
  }
  if(other_member) {
    other_disambiguation = member_disambiguation(*other_member);
  }
  const auto before = vec.size();
  vec.erase(std::remove(vec.begin(), vec.end(), id), vec.end());
  assert(before - vec.size() == 1);
  if(vec.empty()) {
    members_by_displayname_.erase(old_name);
  }
  if(other_member) {
    room->member_disambiguation_changed(*other_member, other_disambiguation);
  }
}

void RoomState::record_displayname(const UserID &id, const QString &name, Room *room) {
  if(name.isEmpty()) return;

  const auto normalized = name.normalized(QString::NormalizationForm_C);
  auto &vec = members_by_displayname_[normalized];
  for(const auto &x : vec) {
    assert(x != id);
  }
  vec.push_back(id);

  if(room) {
    const Member *other_member = nullptr;
    const bool existing_displayname = vec.size() == 2;
    const Member *const existing_mxid = member_from_id(normalized);
    if(!existing_displayname || !existing_mxid) {
      // If there's only one user with the name, they get newly disambiguated too
      if(existing_displayname) other_member = &members_by_id_.at(vec[0]);
      if(existing_mxid) other_member = existing_mxid;
    }
    if(other_member) room->member_disambiguation_changed(*other_member, "");
  }
}

const Member *RoomState::member_from_id(const QString &id) const {
  auto it = members_by_id_.find(id);
  if(it == members_by_id_.end()) return nullptr;
  return &it->second;
}

static constexpr std::chrono::steady_clock::duration MINIMUM_BACKOFF(std::chrono::seconds(5));
// Default synapse seconds-per-message when throttled

Room::Room(Matrix &universe, Session &session, QString id, const QJsonObject &initial,
           lmdb::env &env, lmdb::txn &txn, lmdb::dbi &&member_db)
    : universe_(universe), session_(session), id_(std::move(id)),
      db_env_(env), member_db_(std::move(member_db)), transmitting_(nullptr), retry_backoff_(MINIMUM_BACKOFF)
{
  transmit_retry_timer_.setSingleShot(true);
  connect(&transmit_retry_timer_, &QTimer::timeout, this, &Room::transmit_event);

  if(!initial.isEmpty()) {
    initial_state_ = RoomState(initial["initial_state"].toObject(), txn, member_db_);
    state_ = initial_state_;

    QJsonObject b = initial["buffer"].toObject();
    if(!b.isEmpty()) {
      buffer_.emplace_back();
      buffer_.back().prev_batch = b["prev_batch"].toString();
      buffer_.back().events.reserve(b.size());
      auto es = b["events"].toArray();
      for(const auto &e : es) {
        const auto &evt = parse_event(e);
        buffer_.back().events.emplace_back(evt);
        state_.apply(evt);
        state_.prune_departed();
      }
    }
    highlight_count_ = initial["highlight_count"].toDouble(0);
    notification_count_ = initial["notification_count"].toDouble(0);
    auto receipts = initial["receipts"].toObject();
    for(auto it = receipts.begin(); it != receipts.end(); ++it) {
      update_receipt(it.key(), it.value().toObject()["event_id"].toString(), it.value().toObject()["ts"].toDouble());
    }
  }
}

QString Room::pretty_name() const {
  return state_.pretty_name(session_.user_id());
}

void Room::load_state(lmdb::txn &txn, gsl::span<const proto::Event> events) {
  for(auto &state : events) {
    initial_state_.apply(state);
    initial_state_.prune_departed();
    state_.dispatch(state, this, &member_db_, &txn);
    state_.prune_departed();
  }
}

size_t Room::buffer_size() const {
  size_t r = 0;
  for(auto &x : buffer_) { r += x.events.size(); }
  return r;
}

QJsonObject Room::to_json() const {
  QJsonObject o;
  if(!buffer_.empty()) {
    o["prev_batch"] = buffer_.back().prev_batch;
    QJsonArray es;
    for(const auto &x : buffer_.back().events) {
      auto json = proto::to_json(x);
      es.push_back(std::move(json));
    }
    o["events"] = std::move(es);
  }
  QJsonObject receipts;
  for(const auto &receipt : receipts_by_user_) {
    receipts[receipt.first] = QJsonObject{{"event_id", receipt.second.event}, {"ts", static_cast<qint64>(receipt.second.ts)}};
  }
  return QJsonObject{
    {"initial_state", initial_state_.to_json()},
    {"buffer", std::move(o)},
    {"highlight_count", static_cast<double>(highlight_count_)},
    {"notification_count", static_cast<double>(notification_count_)},
    {"receipts", receipts}
  };
}

bool Room::dispatch(lmdb::txn &txn, const proto::JoinedRoom &joined) {
  bool state_touched = false;

  if(joined.unread_notifications.highlight_count != highlight_count_) {
    auto old = highlight_count_;
    highlight_count_ = joined.unread_notifications.highlight_count;
    highlight_count_changed(old);
  }

  if(joined.unread_notifications.notification_count != notification_count_) {
    auto old = notification_count_;
    notification_count_ = joined.unread_notifications.notification_count;
    notification_count_changed(old);
  }

  if(joined.timeline.limited) {
    buffer_.clear();
    discontinuity();
  }

  prev_batch(joined.timeline.prev_batch);
  // Must be called *after* discontinuity so that users can easily discard existing timeline events

  // Ensure that only the first batch in the buffer can ever be empty
  if(joined.timeline.events.empty() && !buffer_.empty()) {
    buffer_.back().prev_batch = joined.timeline.prev_batch;
  } else {
    buffer_.emplace_back();     // In-place so has_unread is always up to date
    auto &batch = buffer_.back();
    batch.prev_batch = joined.timeline.prev_batch;
    batch.events.reserve(joined.timeline.events.size());
    for(auto &evt : joined.timeline.events) {
      state_touched |= state_.dispatch(evt, this, &member_db_, &txn);

      // Must be placed before `message` so resulting calls to `has_unread` return accurate results accounting for the
      // message in question
      batch.events.emplace_back(evt);

      message(evt);

      // Must happen after we dispatch the previous event but before we process the next one, to ensure display names are
      // correct for leave/ban events as well as whatever follows
      state_.prune_departed(this);
    }

    while(!buffer_.empty() && (buffer_size() - buffer_.front().events.size()) >= session_.buffer_size()) {
      for(auto &evt : buffer_.front().events) {
        initial_state_.apply(evt);
        initial_state_.prune_departed();
      }
      buffer_.pop_front();
    }
  }

  for(const auto &evt : joined.ephemeral.events) {
    if(evt.type == "m.receipt") {
      for(auto read_evt = evt.content.begin(); read_evt != evt.content.end(); ++read_evt) {
        const auto &obj = read_evt.value().toObject()["m.read"].toObject();
        for(auto user = obj.begin(); user != obj.end(); ++user) {
          update_receipt(user.key(), read_evt.key(), user.value().toObject()["ts"].toDouble());
        }
      }
      receipts_changed();
    } else if(evt.type == "m.typing") {
      typing_.clear();
      const auto ids = evt.content["user_ids"].toArray();
      for(const auto &x : ids) {
        typing_.push_back(x.toString());
      }
      typing_changed();
    } else {
      qDebug() << "Unrecognized ephemeral event type:" << evt.type;
    }
  }

  if(state_touched) {
    state_changed();
  }
  return state_touched;
}

bool RoomState::update_membership(const QString &user_id, const QJsonObject &content, Room *room,
                                  lmdb::dbi *member_db, lmdb::txn *txn) {
  Membership membership;
  if(content.empty()) {
    // Empty content arises when moving backwards from an initial event
    membership = Membership::LEAVE;
  } else {
    auto r = parse_membership(content["membership"].toString());
    if(!r) {
      qDebug() << "Unrecognized membership type" << content["membership"].toString();
      return false;
    }
    membership = *r;
  }

  auto id_utf8 = user_id.toUtf8();

  switch(membership) {
  case Membership::INVITE:
  case Membership::JOIN: {
    auto &member = members_by_id_.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(user_id),
        std::forward_as_tuple(user_id)).first->second;
    auto old_membership = member.membership();
    auto old_displayname = member.display_name();
    auto old_member_name = member_name(member);
    member.update_membership(content);
    if(member.display_name() != old_displayname) {
      forget_displayname(member.id(), old_displayname, room);
      record_displayname(member.id(), member.display_name(), room);
      if(room && membership_displayable(old_membership)) room->member_name_changed(member, old_member_name);
    }
    if(room && member.membership() != old_membership) {
      room->membership_changed(member, membership);
    }
    if(member_db) {
      auto data = QJsonDocument(member.to_json()).toBinaryData();
      lmdb::dbi_put(*txn, *member_db, lmdb::val(id_utf8.data(), id_utf8.size()),
                    lmdb::val(data.data(), data.size()));
    }
    break;
  }
  case Membership::LEAVE:
  case Membership::BAN: {
    if(room && user_id == room->id()) {
      room->left(membership);
    }
    auto it = members_by_id_.find(user_id);
    if(it != members_by_id_.end()) {
      auto &member = it->second;
      auto old_displayname = member.display_name();
      member.update_membership(content);
      if(member.display_name() != old_displayname) {
        forget_displayname(member.id(), old_displayname, room);
        record_displayname(member.id(), member.display_name(), room);
      }
      if(room) room->membership_changed(member, membership);
      assert(departed_.isEmpty());
      departed_ = member.id();
    }
    if(member_db) {
      lmdb::dbi_del(*txn, *member_db, lmdb::val(id_utf8.data(), id_utf8.size()), nullptr);
    }
    break;
  }
  }
  return true;
}

bool RoomState::dispatch(const proto::Event &state, Room *room, lmdb::dbi *member_db, lmdb::txn *txn) {
  if(state.type == "m.room.message") return false;
  if(state.type == "m.room.aliases") {
    std::unordered_set<QString, QStringHash> all_aliases;
    auto data = state.content["aliases"].toArray();  // FIXME: Need to validate these before using them
    all_aliases.reserve(aliases_.size() + data.size());

    std::move(aliases_.begin(), aliases_.end(), std::inserter(all_aliases, all_aliases.end()));
    aliases_.clear();

    std::transform(data.begin(), data.end(), std::inserter(all_aliases, all_aliases.end()),
                   [](const QJsonValue &v){ return v.toString(); });

    aliases_.reserve(all_aliases.size());
    std::move(all_aliases.begin(), all_aliases.end(), std::back_inserter(aliases_));
    if(room) room->aliases_changed();
    return true;
  }
  if(state.type == "m.room.canonical_alias") {
    auto old = std::move(canonical_alias_);
    canonical_alias_ = state.content["alias"].toString();
    if(room && canonical_alias_ != old) room->canonical_alias_changed();
    return true;
  }
  if(state.type == "m.room.name") {
    auto old = std::move(name_);
    name_ = state.content["name"].toString();
    if(room && name_ != old) room->name_changed();
    return true;
  }
  if(state.type == "m.room.topic") {
    auto old = std::move(topic_);
    topic_ = state.content["topic"].toString();
    if(room && topic_ != old) {
      room->topic_changed(old);
    }
    return true;
  }
  if(state.type == "m.room.avatar") {
    auto old = std::move(avatar_);
    avatar_ = QUrl(state.content["url"].toString());
    if(room && avatar_ != old) room->avatar_changed();
    return true;
  }
  if(state.type == "m.room.create") {
    // Nothing to do here, because our rooms data structures are created implicitly
    return false;
  }
  if(state.type == "m.room.member") {
    return update_membership(state.state_key, state.content, room, member_db, txn);
  }

  qDebug() << "Unrecognized message type:" << state.type;
  return false;
}

void RoomState::revert(const proto::Event &state) {
  if(state.type == "m.room.message") return;
  if(state.type == "m.room.canonical_alias") {
    if(state.unsigned_.prev_content)
      canonical_alias_ = state.unsigned_.prev_content.value()["alias"].toString();
    else
      canonical_alias_ = QString();
    return;
  }
  if(state.type == "m.room.name") {
    if(state.unsigned_.prev_content)
      name_ = state.unsigned_.prev_content.value()["name"].toString();
    else
      name_ = QString();
    return;
  }
  if(state.type == "m.room.topic") {
    auto old = std::move(topic_);
    if(state.unsigned_.prev_content)
      topic_ = (*state.unsigned_.prev_content)["topic"].toString();
    else
      topic_ = QString();
    return;
  }
  if(state.type == "m.room.avatar") {
    if(state.unsigned_.prev_content)
      avatar_ = QUrl((*state.unsigned_.prev_content)["url"].toString());
    else
      avatar_ = QUrl();
    return;
  }
  if(state.type == "m.room.member") {
    update_membership(state.state_key, state.unsigned_.prev_content.value_or(QJsonObject()), nullptr, nullptr, nullptr);
    prune_departed();
    return;
  }
}

void RoomState::ensure_member(const proto::Event &e) {
  if(e.type != "m.room.member") return;
  auto r = parse_membership(e.content["membership"].toString());
  if(!r) {
    qDebug() << "Unrecognized membership type" << e.content["membership"].toString();
    return;
  }
  switch(*r) {
  case Membership::LEAVE:
  case Membership::BAN: {
    auto r = members_by_id_.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(e.state_key),
      std::forward_as_tuple(e.state_key));
    if(!r.second) break;
    auto &member = r.first->second;
    if(e.unsigned_.prev_content) {
      // Ensure that we get display name and avatar, if available
      member.update_membership(*e.unsigned_.prev_content);
    }
    member.update_membership(e.content);
    record_displayname(member.id(), member.display_name(), nullptr);
  }
  default:
    break;
  }
}

void RoomState::prune_departed(Room *room) {
  if(!departed_.isEmpty()) {
    forget_displayname(departed_, members_by_id_.at(departed_).display_name(), room);
    members_by_id_.erase(departed_);
    departed_ = QString();
  }
}

MessageFetch *Room::get_messages(Direction dir, QString from, uint64_t limit, QString to) {
  QUrlQuery query;
  query.addQueryItem("from", from);
  query.addQueryItem("dir", dir == Direction::FORWARD ? "f" : "b");
  if(limit != 0) query.addQueryItem("limit", QString::number(limit));
  if(!to.isEmpty()) query.addQueryItem("to", to);
  auto reply = session_.get(QString("client/r0/rooms/" % QUrl::toPercentEncoding(id_) % "/messages"), query);
  auto result = new MessageFetch(reply);
  connect(reply, &QNetworkReply::finished, [reply, result]() {
      auto r = decode(reply);
      if(r.error) {
        result->error(*r.error);
        return;
      }
      auto start_val = r.object["start"];
      if(!start_val.isString()) {
        result->error("invalid or missing \"start\" attribute in server's response");
        return;
      }
      auto start = start_val.toString();
      auto end_val = r.object["end"];
      if(!end_val.isString()) {
        result->error("invalid or missing \"end\" attribute in server's response");
        return;
      }
      auto end = end_val.toString();
      auto chunk_val = r.object["chunk"];
      if(!chunk_val.isArray()) {
        result->error("invalid or missing \"chunk\" attribute in server's response");
        return;
      }
      auto chunk = chunk_val.toArray();
      std::vector<proto::Event> events;
      events.reserve(chunk.size());
      std::transform(chunk.begin(), chunk.end(), std::back_inserter(events), parse_event);
      result->finished(start, end, events);
    });
  return result;
}

EventSend *Room::leave() {
  auto reply = session_.post(QString("client/r0/rooms/" % QUrl::toPercentEncoding(id_) % "/leave"));
  auto es = new EventSend(reply);
  connect(reply, &QNetworkReply::finished, [es, reply]() {
      auto r = decode(reply);
      if(r.error) {
        es->error(*r.error);
      } else {
        es->finished();
      }
    });
  return es;
}

void Room::send(const QString &type, QJsonObject content) {
  pending_events_.push_back({type, content});
  transmit_event();
}

void Room::redact(const EventID &event, const QString &reason) {
  auto txn = session_.get_transaction_id();
  auto reply = session_.put("client/r0/rooms/" % QUrl::toPercentEncoding(id_) % "/redact/" % QUrl::toPercentEncoding(event) % "/" % txn,
                            reason.isEmpty() ? QJsonObject() : QJsonObject{{"reason", reason}}
    );
  auto es = new EventSend(reply);
  connect(reply, &QNetworkReply::finished, [reply, es]() {
      if(reply->error()) es->error(reply->errorString());
      else es->finished();
    });
  connect(es, &EventSend::error, this, &Room::error);
}

void Room::send_file(const QString &uri, const QString &name, const QString &media_type, size_t size) {
  send("m.room.message",
       {{"msgtype", "m.file"},
         {"url", uri},
         {"filename", name},
         {"body", name},
         {"info", QJsonObject{
             {"mimetype", media_type},
             {"size", static_cast<qint64>(size)}}}
           });
}

void Room::send_message(const QString &body) {
  send("m.room.message",
       {{"msgtype", "m.text"},
         {"body", body}});
}

void Room::send_emote(const QString &body) {
  send("m.room.message",
       {{"msgtype", "m.emote"},
         {"body", body}});
}

void Room::send_read_receipt(const EventID &event) {
  auto reply = session_.post(QString("client/r0/rooms/" % QUrl::toPercentEncoding(id_) % "/receipt/m.read/" % QUrl::toPercentEncoding(event)));
  auto es = new EventSend(reply);
  connect(reply, &QNetworkReply::finished, [reply, es, event]() {
      if(reply->error()) {
        es->error(reply->errorString());
      } else {
        es->finished();
      }
    });
  connect(es, &EventSend::error, this, &Room::error);
}

gsl::span<const Room::Receipt * const> Room::receipts_for(const EventID &id) const {
  auto it = receipts_by_event_.find(id);
  if(it == receipts_by_event_.end()) return {};
  return gsl::span<const Room::Receipt * const>(static_cast<const Receipt * const *>(it->second.data()),
                                                  it->second.size());
}

const Room::Receipt *Room::receipt_from(const UserID &id) const {
  auto it = receipts_by_user_.find(id);
  if(it == receipts_by_user_.end()) return nullptr;
  return &it->second;
}

bool Room::has_unread() const {
  if(buffer().empty() || buffer().back().events.empty()) return true;
  auto r = receipt_from(session().user_id());
  if(!r) return true;
  for(auto batch = buffer().crbegin(); batch != buffer().crend(); ++batch) {
    for(auto event = batch->events.crbegin(); event != batch->events.crend(); ++event) {
      if(r->event == event->event_id) return false;
      if(event->type == "m.room.message" && event->sender != session().user_id()) return true;
    }
  }
  return true;
}

void Room::update_receipt(const UserID &user, const EventID &event, uint64_t ts) {
  const Receipt new_value{event, ts};
  auto emplaced = receipts_by_user_.emplace(user, new_value);
  if(!emplaced.second) {
    auto it = receipts_by_event_.find(emplaced.first->second.event);
    if(it != receipts_by_event_.end()) {
      auto &vec = it->second;
      // Remove from index
      vec.erase(std::remove(vec.begin(), vec.end(), &emplaced.first->second), vec.end());
    }
    emplaced.first->second = new_value;
  }
  receipts_by_event_[event].push_back(&emplaced.first->second);
}

void Room::transmit_event() {
  if(transmitting_) return;     // We'll be re-invoked when necessary by transmit_finished

  const auto &event = pending_events_.front();
  if(last_transmit_transaction_.isEmpty()) last_transmit_transaction_ = session_.get_transaction_id();
  transmitting_ = session_.put(
    "client/r0/rooms/" % QUrl::toPercentEncoding(id_) % "/send/" % QUrl::toPercentEncoding(event.type) % "/" % last_transmit_transaction_,
    event.content);
  connect(transmitting_, &QNetworkReply::finished, this, &Room::transmit_finished);
}

void Room::transmit_finished() {
  using namespace std::chrono;
  using namespace std::chrono_literals;

  auto r = decode(transmitting_);
  transmitting_ = nullptr;
  bool retrying = false;
  if(r.code >= 400 && r.code < 500 && r.code != 429) {
    // HTTP client errors other than rate-limiting are unrecoverable
    error(*r.error);
    pending_events_.pop_front();
  } else if(!r.error) {
    pending_events_.pop_front();
  } else {
    retrying = true;
    qDebug() << "retrying send in" << duration_cast<duration<float>>(retry_backoff_).count() << "seconds due to error:" << *r.error;
  }
  if(!retrying) {
    last_transmit_transaction_ = "";
    retry_backoff_ = MINIMUM_BACKOFF;
  }
  if(!pending_events_.empty()) {
    if(retrying) {
      transmit_retry_timer_.start(duration_cast<milliseconds>(retry_backoff_).count());
      retry_backoff_ = std::min<steady_clock::duration>(30s, duration_cast<steady_clock::duration>(1.25 * retry_backoff_));
    } else {
      transmit_event();
    }
  }
}

}
