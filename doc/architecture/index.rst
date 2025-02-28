Architecture
============

.. panels::

    :fa:`book` :doc:`consensus/index`
    ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

    CCF makes use of a consensus protocol to replicate transactions.

    ---

    :fa:`key` :doc:`cryptography`
    ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

    CCF leverages a number of cryptographic techniques and primitives.

    ---

    :fa:`stream` :doc:`threading`
    ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

    CCF threading model.

    ---

    :fa:`project-diagram` :doc:`merkle_tree`
    ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

    Transaction integrity guarantees stem from the use of a Merkle Tree data structure over their history.

    ---

    :fa:`book` :doc:`ledger`
    ^^^^^^^^^^^^^^^^^^^^^^^^

    Transactions are persisted to a ledger for recovery and audit purposes.

    ---

    :fa:`book` :doc:`raft_tla`
    ^^^^^^^^^^^^^^^^^^^^^^^^^^

    A TLA+ model of Raft as modified when implemented in CCF.

    ---

    :fa:`paper-plane` :doc:`node_to_node`
    ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

    Node to node channel protocol used for consensus and forwarding.

    ---

    :fa:`address-book` :doc:`indexing`
    ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

    Indexing system used to speed up historical queries.

    ---

    :fa:`scroll` :doc:`receipts`
    ^^^^^^^^^^^^^^^^^^^^^^^^^^^

    Receipts can be used with the ledger for audit purposes.


    ---

    :fa:`address-book` :doc:`tls_internals`
    ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

    Internal implementation of TLS communication.

    ---

    :fa:`address-book` :doc:`tcp_internals`
    ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

    Internal implementation of the TCP host layer.

    ---

    :fa:`address-book` :doc:`quic_internals`
    ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

    Internal implementation of QUIC communication (in progress).

.. toctree::
    :hidden:
    :maxdepth: 1

    consensus/index
    cryptography
    threading
    merkle_tree
    ledger
    raft_tla
    node_to_node
    indexing
    receipts
    tls_internals
    tcp_internals
    quic_internals
