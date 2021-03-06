/*  =========================================================================
    dafka_consumer -

    Copyright (c) the Contributors as noted in the AUTHORS file.
    This file is part of CZMQ, the high-level C binding for 0MQ:
    http://czmq.zeromq.org.

    This Source Code Form is subject to the terms of the Mozilla Public
    License, v. 2.0. If a copy of the MPL was not distributed with this
    file, You can obtain one at http://mozilla.org/MPL/2.0/.
    =========================================================================
*/

/*
@header
    dafka_consumer -
@discuss
    TODO:
      - Send earliest message when a store connects
      - We must not send FETCH on every message, the problem is, that if you
        missed something, and there is high rate, you will end up sending a
        lot of fetch messages for same address
      - Prioritize DIRECT_MSG messages over MSG this will avoid discrding MSGs
        when catching up
@end
*/

#include "dafka_classes.h"

//  Structure of our actor

struct _dafka_consumer_t {
    //  Actor properties
    zsock_t *pipe;              //  Actor command pipe
    zpoller_t *poller;          //  Socket poller
    bool terminated;            //  Did caller ask us to quit?
    bool verbose;               //  Verbose logging enabled?
    //  Class properties
    zsock_t *consumer_sub;      // Subscriber to get messages from topics
    dafka_proto_t *consumer_msg;// Reusable consumer message

    zsock_t *consumer_pub;      // Publisher to ask for missed messages
    zhashx_t *sequence_index;   // Index containing the latest sequence for each
                                // known publisher
    dafka_proto_t *fetch_msg;   // Reusable fetch message
    dafka_proto_t *earlist_msg; // Reusable earliest message
    zactor_t* beacon;           // Beacon actor
    bool reset_latest;          // Wheather to process records from earliest or latest
};

//  --------------------------------------------------------------------------
//  Create a new dafka_consumer instance

static dafka_consumer_t *
dafka_consumer_new (zsock_t *pipe, zconfig_t *config)
{
    dafka_consumer_t *self = (dafka_consumer_t *) zmalloc (sizeof (dafka_consumer_t));
    assert (self);

    //  Initialize actor properties
    self->pipe = pipe;
    self->terminated = false;
    self->reset_latest = streq (zconfig_get (config, "consumer/offset/reset", "latest"), "latest");

    //  Initialize class properties
    if (atoi (zconfig_get (config, "consumer/verbose", "0")))
        self->verbose = true;

    self->consumer_sub = zsock_new_sub (NULL, NULL);
    self->consumer_msg = dafka_proto_new ();

    self->sequence_index = zhashx_new ();
    zhashx_set_destructor(self->sequence_index, uint64_destroy);
    zhashx_set_duplicator (self->sequence_index, uint64_dup);

    self->consumer_pub = zsock_new_pub (NULL);
    int port = zsock_bind (self->consumer_pub, "tcp://*:*");
    assert (port != -1);

    zuuid_t *consumer_address = zuuid_new ();
    self->earlist_msg = dafka_proto_new ();
    dafka_proto_set_id (self->earlist_msg, DAFKA_PROTO_EARLIEST);
    dafka_proto_set_address (self->earlist_msg, zuuid_str (consumer_address));

    self->fetch_msg = dafka_proto_new ();
    dafka_proto_set_id (self->fetch_msg, DAFKA_PROTO_FETCH);
    dafka_proto_set_address (self->fetch_msg, zuuid_str (consumer_address));

    dafka_proto_subscribe (self->consumer_sub, DAFKA_PROTO_DIRECT_MSG, zuuid_str (consumer_address));

    self->beacon = zactor_new (dafka_beacon_actor, config);
    zsock_send (self->beacon, "ssi", "START", zuuid_str (consumer_address), port);
    assert (zsock_wait (self->beacon) == 0);
    zuuid_destroy(&consumer_address);

    self->poller = zpoller_new (self->pipe, self->consumer_sub, self->beacon, NULL);

    return self;
}


//  --------------------------------------------------------------------------
//  Destroy the dafka_consumer instance

static void
dafka_consumer_destroy (dafka_consumer_t **self_p)
{
    assert (self_p);
    if (*self_p) {
        dafka_consumer_t *self = *self_p;

        //  Free class properties
        zpoller_destroy (&self->poller);
        zsock_destroy (&self->consumer_sub);
        zsock_destroy (&self->consumer_pub);

        dafka_proto_destroy (&self->consumer_msg);
        dafka_proto_destroy (&self->earlist_msg);
        dafka_proto_destroy (&self->fetch_msg);
        zhashx_destroy (&self->sequence_index);
        zactor_destroy (&self->beacon);

        //  Free actor properties
        self->terminated = true;
        free (self);
        *self_p = NULL;
    }
}


//  Subscribe this actor to an topic. Return a value greater or equal to zero if
//  was successful. Otherwise -1.

static void
s_subscribe (dafka_consumer_t *self, const char *topic)
{
    assert (self);
    if (self->verbose)
        zsys_debug ("Consumer: Subscribe to topic %s", topic);

    dafka_proto_subscribe (self->consumer_sub, DAFKA_PROTO_MSG, topic);
    dafka_proto_subscribe (self->consumer_sub, DAFKA_PROTO_HEAD, topic);

    if (!self->reset_latest) {
        if (self->verbose)
            zsys_debug ("Consumer: Send EARLIEST message for topic %s", topic);

        dafka_proto_set_topic (self->earlist_msg, topic);
        dafka_proto_send (self->earlist_msg, self->consumer_pub);
    }
}


//  Here we handle incoming message from the subscribtions

static void
dafka_consumer_recv_subscriptions (dafka_consumer_t *self)
{
    int rc = dafka_proto_recv (self->consumer_msg, self->consumer_sub);
    if (rc != 0)
       return;        //  Interrupted

    char id = dafka_proto_id (self->consumer_msg);
    zframe_t *content = dafka_proto_content (self->consumer_msg);
    uint64_t msg_sequence = dafka_proto_sequence (self->consumer_msg);

    const char *address;
    const char *subject;
    if (id == DAFKA_PROTO_MSG || id == DAFKA_PROTO_DIRECT_MSG) {
        address = dafka_proto_address (self->consumer_msg);
        subject = dafka_proto_subject (self->consumer_msg);
    }
    else
    if (id == DAFKA_PROTO_HEAD) {
        address = dafka_proto_address (self->consumer_msg);
        subject = dafka_proto_subject (self->consumer_msg);
    }
    else
        return;     // Unexpected message id

    // TODO: Extract into struct and/or add zstr_concat
    char *sequence_key = (char *) malloc (strlen (address) + strlen (subject) + 2);
    strcpy (sequence_key, subject);
    strcat (sequence_key, "/");
    strcat (sequence_key, address);

    if (self->verbose)
        zsys_debug ("Consumer: Received message %c from %s on subject %s with sequence %u",
                    id, address, subject, msg_sequence);

    //  Check if we missed some messages
    // TODO: If unkown send earliest
    uint64_t last_known_sequence = -1;
    bool last_known = zhashx_lookup (self->sequence_index, sequence_key);
    if (last_known)
        last_known_sequence = *((uint64_t *) zhashx_lookup (self->sequence_index, sequence_key));
    else
    if (self->reset_latest) {
        if (self->verbose)
            zsys_debug ("Consumer: Setting offset for topic %s on partition %s to latest %u",
                        subject,
                        address,
                        msg_sequence - 1);

        if (id == DAFKA_PROTO_MSG || id == DAFKA_PROTO_DIRECT_MSG)
            // Set to latest - 1 in order to process the current message
            last_known_sequence = msg_sequence - 1;
        else
        if (id == DAFKA_PROTO_HEAD)
            // Set to latest in order to skip fetching older messages
            last_known_sequence = msg_sequence;

        zhashx_insert (self->sequence_index, sequence_key, &last_known_sequence);
    }

    // TODO: I'm so ugly and complicated please make me pretty
    if (((id == DAFKA_PROTO_MSG || id == DAFKA_PROTO_DIRECT_MSG) && (!last_known || msg_sequence > last_known_sequence + 1)) ||
        (id == DAFKA_PROTO_HEAD && (!last_known || msg_sequence > last_known_sequence))) {
        uint64_t no_of_missed_messages = msg_sequence - last_known_sequence;
        if (self->verbose)
            zsys_debug ("Consumer: FETCHING %u messages on subject %s from %s starting at sequence %u",
                        no_of_missed_messages,
                        subject,
                        address,
                        last_known_sequence + 1);

        dafka_proto_set_subject (self->fetch_msg, subject);
        dafka_proto_set_topic (self->fetch_msg, address);
        dafka_proto_set_sequence (self->fetch_msg, last_known_sequence + 1);
        dafka_proto_set_count (self->fetch_msg, no_of_missed_messages);
        dafka_proto_send (self->fetch_msg, self->consumer_pub);
    }

    if (id == DAFKA_PROTO_MSG || id == DAFKA_PROTO_DIRECT_MSG) {
        if (msg_sequence == last_known_sequence + 1) {
            if (self->verbose)
                zsys_debug ("Consumer: Send message %u to client", msg_sequence);

            zhashx_update (self->sequence_index, sequence_key, &msg_sequence);
            zsock_bsend (self->pipe, "ssf", subject, address, content);
        }
    }

    zstr_free (&sequence_key);
}

//  Here we handle incoming message from the node

static void
dafka_consumer_recv_api (dafka_consumer_t *self)
{
    //  Get the whole message of the pipe in one go
    zmsg_t *request = zmsg_recv (self->pipe);
    if (!request)
       return;        //  Interrupted

    char *command = zmsg_popstr (request);
    if (streq (command, "SUBSCRIBE")) {
        char *topic = zmsg_popstr (request);
        s_subscribe (self, topic);
        zstr_free (&topic);
    }
    else
    if (streq (command, "$TERM"))
        //  The $TERM command is send by zactor_destroy() method
        self->terminated = true;
    else {
        zsys_error ("invalid command '%s'", command);
        assert (false);
    }
    zstr_free (&command);
    zmsg_destroy (&request);
}


//  --------------------------------------------------------------------------
//  This is the actor which runs in its own thread.

void
dafka_consumer (zsock_t *pipe, void *args)
{
    dafka_consumer_t * self = dafka_consumer_new (pipe, (zconfig_t *) args);
    if (!self)
        return;          //  Interrupted

    //  Signal actor successfully initiated
    zsock_signal (self->pipe, 0);

    if (self->verbose)
        zsys_info ("Consumer: running...");

    while (!self->terminated) {
        void *which = (zsock_t *) zpoller_wait (self->poller, -1);
        if (which == self->pipe)
            dafka_consumer_recv_api (self);
        if (which == self->consumer_sub)
            dafka_consumer_recv_subscriptions (self);
        if (which == self->beacon)
            dafka_beacon_recv (self->beacon, self->consumer_sub, self->verbose, "Consumer");
    }
    bool verbose = self->verbose;
    dafka_consumer_destroy (&self);

    if (verbose)
        zsys_info ("Consumer: stopped");
}

//  --------------------------------------------------------------------------
//  Subscribe to a topic

int
dafka_consumer_subscribe (zactor_t* actor, const char* subject) {
    return zsock_send (actor, "ss", "SUBSCRIBE", subject);
}

//  --------------------------------------------------------------------------
//  Self test of this actor.

// If your selftest reads SCMed fixture data, please keep it in
// src/selftest-ro; if your test creates filesystem objects, please
// do so under src/selftest-rw.
// The following pattern is suggested for C selftest code:
//    char *filename = NULL;
//    filename = zsys_sprintf ("%s/%s", SELFTEST_DIR_RO, "mytemplate.file");
//    assert (filename);
//    ... use the "filename" for I/O ...
//    zstr_free (&filename);
// This way the same "filename" variable can be reused for many subtests.
#define SELFTEST_DIR_RO "src/selftest-ro"
#define SELFTEST_DIR_RW "src/selftest-rw"

void
dafka_consumer_test (bool verbose)
{
    printf (" * dafka_consumer: ");
    //  @selftest
    // ----------------------------------------------------
    // Test with consumer.offset.reset = earliest
    // ----------------------------------------------------
    zconfig_t *config = zconfig_new ("root", NULL);
    zconfig_put(config, "beacon/interval", "50");
    zconfig_put (config, "beacon/verbose", verbose ? "1" : "0");
    zconfig_put (config, "beacon/sub_address", "inproc://consumer-tower-sub");
    zconfig_put (config, "beacon/pub_address", "inproc://consumer-tower-pub");
    zconfig_put (config, "tower/verbose", verbose ? "1" : "0");
    zconfig_put (config, "tower/sub_address", "inproc://consumer-tower-sub");
    zconfig_put (config, "tower/pub_address", "inproc://consumer-tower-pub");
    zconfig_put (config, "consumer/verbose", verbose ? "1" : "0");
    zconfig_put (config, "consumer/offset/reset", "earliest");
    zconfig_put (config, "producer/verbose", verbose ? "1" : "0");
    zconfig_put (config, "store/verbose", verbose ? "1" : "0");
    zconfig_put (config, "store/db", SELFTEST_DIR_RW "/storedb");

    zactor_t *tower = zactor_new (dafka_tower_actor, config);

    dafka_producer_args_t pub_args = { "hello", config };
    zactor_t *producer =  zactor_new (dafka_producer, &pub_args);
    assert (producer);

    zactor_t *store = zactor_new (dafka_store_actor, config);
    assert (store);

    zactor_t *consumer = zactor_new (dafka_consumer, config);
    assert (consumer);
    zclock_sleep (250);

    dafka_producer_msg_t *p_msg = dafka_producer_msg_new ();
    dafka_producer_msg_set_content_str (p_msg , "HELLO MATE");
    int rc = dafka_producer_msg_send (p_msg, producer);
    assert (rc == 0);
    zclock_sleep (100);  // Make sure message is published before consumer subscribes

    rc = dafka_consumer_subscribe (consumer, "hello");
    assert (rc == 0);
    zclock_sleep (250);  // Make sure subscription is active before sending the next message

    // This message is discarded but triggers a FETCH from the store
    dafka_producer_msg_set_content_str (p_msg, "HELLO ATEM");
    rc = dafka_producer_msg_send (p_msg, producer);
    assert (rc == 0);
    zclock_sleep (100);  // Make sure the first two messages have been received from the store and the consumer is now up to date

    dafka_producer_msg_set_content_str (p_msg, "HELLO TEMA");
    rc = dafka_producer_msg_send (p_msg, producer);
    assert (rc == 0);

    // Receive the first message from the STORE
    dafka_consumer_msg_t *c_msg = dafka_consumer_msg_new ();
    dafka_consumer_msg_recv (c_msg, consumer);
    assert (streq (dafka_consumer_msg_subject (c_msg), "hello"));
    assert (dafka_consumer_msg_streq (c_msg, "HELLO MATE"));

    // Receive the second message from the STORE as the original has been discarded
    dafka_consumer_msg_recv (c_msg, consumer);
    assert (streq (dafka_consumer_msg_subject (c_msg), "hello"));
    assert (dafka_consumer_msg_streq (c_msg, "HELLO ATEM"));

    // Receive the third message from the PUBLISHER
    dafka_consumer_msg_recv (c_msg, consumer);
    assert (streq (dafka_consumer_msg_subject (c_msg), "hello"));
    assert (dafka_consumer_msg_streq (c_msg, "HELLO TEMA"));

    dafka_producer_msg_destroy (&p_msg);
    dafka_consumer_msg_destroy (&c_msg);
    zactor_destroy (&producer);
    zactor_destroy (&store);
    zactor_destroy (&consumer);

    // ----------------------------------------------------
    // Test with consumer.offset.reset = latest
    // ----------------------------------------------------
    zconfig_put (config, "consumer/offset/reset", "latest");

    producer =  zactor_new (dafka_producer, &pub_args);
    assert (producer);

    consumer = zactor_new (dafka_consumer, config);
    assert (consumer);
    zclock_sleep (250);

    //  This message is missed by the consumer and later ignored because the
    //  offset reset is set to latest.
    p_msg = dafka_producer_msg_new ();
    dafka_producer_msg_set_content_str (p_msg , "HELLO MATE");
    rc = dafka_producer_msg_send (p_msg, producer);
    assert (rc == 0);
    zclock_sleep (100);  // Make sure message is published before consumer subscribes

    rc = dafka_consumer_subscribe (consumer, "hello");
    assert (rc == 0);
    zclock_sleep (250);  // Make sure subscription is active before sending the next message

    dafka_producer_msg_set_content_str (p_msg , "HELLO ATEM");
    rc = dafka_producer_msg_send (p_msg, producer);
    assert (rc == 0);

    // Receive the second message from the PRODUCER
    c_msg = dafka_consumer_msg_new ();
    dafka_consumer_msg_recv (c_msg, consumer);
    assert (streq (dafka_consumer_msg_subject (c_msg), "hello"));
    assert (dafka_consumer_msg_streq (c_msg, "HELLO ATEM"));

    dafka_producer_msg_destroy (&p_msg);
    dafka_consumer_msg_destroy (&c_msg);
    zactor_destroy (&tower);
    zactor_destroy (&producer);
    zactor_destroy (&consumer);
    zconfig_destroy (&config);
    //  @end

    printf ("OK\n");
}
