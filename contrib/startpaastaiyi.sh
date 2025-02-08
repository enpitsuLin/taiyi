#!/bin/bash

VERSION=`cat /etc/taiyiversion`

if [[ "$IS_BROADCAST_NODE" ]]; then
  TAIYIN="/usr/local/taiyi-default/bin/taiyin"
elif [[ "$IS_AH_NODE" ]]; then
  TAIYIN="/usr/local/taiyi-default/bin/taiyin"
else
  TAIYIN="/usr/local/taiyi-full/bin/taiyin"
fi

if [[ "$SYNC_TO_S3" ]]; then
  echo "[info] create issyncnode file."
  touch /tmp/issyncnode
  chown www-data:www-data /tmp/issyncnode
fi

# add a tag file to help check if download process has been done.
echo "[info] create downloading tag file."
touch /tmp/isdownloading

# start nginx before downloading backup file.
# because the max rolling update timeout is 1 hour,
# but the download process is longer than 1 hour.
cp /etc/nginx/healthcheck.conf.template /etc/nginx/healthcheck.conf
# suppose we have healthy nodes in the auto scaling group
echo server 127.0.0.1:8091\; >> /etc/nginx/healthcheck.conf
echo } >> /etc/nginx/healthcheck.conf
rm /etc/nginx/sites-enabled/default
cp /etc/nginx/healthcheck.conf /etc/nginx/sites-enabled/default
/etc/init.d/fcgiwrap restart
service nginx restart
echo "[info] nginx started."

chown -R taiyi:taiyi $HOME

# clean out data dir since it may be semi-persistent block storage on the ec2 with stale data
rm -rf $HOME/*

ARGS=""

# if user did pass in desired seed nodes, use
# the ones the user specified:
if [[ ! -z "$TAIYI_SEED_NODES" ]]; then
    for NODE in $TAIYI_SEED_NODES ; do
        ARGS+=" --p2p-seed-node=$NODE"
    done
fi

if [[ ! "$DISABLE_BLOCK_API" ]]; then
   ARGS+=" --plugin=block_api"
fi

# overwrite local config with image one
if [[ "$IS_BROADCAST_NODE" ]]; then
  cp /etc/taiyi/config-for-broadcaster.ini $HOME/config.ini
elif [[ "$IS_AH_NODE" ]]; then
  cp /etc/taiyi/config-for-ahnode.ini $HOME/config.ini
elif [[ "$IS_OPSWHITELIST_NODE" ]]; then
  cp /etc/taiyi/fullnode.opswhitelist.config.ini $HOME/config.ini
else
  cp /etc/taiyi/fullnode.config.ini $HOME/config.ini
fi

chown taiyi:taiyi $HOME/config.ini

cd $HOME

mv /etc/nginx/nginx.conf /etc/nginx/nginx.original.conf
cp /etc/nginx/taiyi.nginx.conf /etc/nginx/nginx.conf

# get blockchain state from an S3 bucket
finished=0
count=1
if [[ "$USE_RAMDISK" ]]; then
  mkdir -p /mnt/ramdisk
  mount -t ramfs -o size=${RAMDISK_SIZE_IN_MB:-51200}m ramfs /mnt/ramdisk
  ARGS+=" --shared-file-dir=/mnt/ramdisk/blockchain"
  # try five times to pull in shared memory file
  while [[ $count -le 5 ]] && [[ $finished == 0 ]]
  do
    rm -rf $HOME/blockchain/*
    rm -rf /mnt/ramdisk/blockchain/*
    if [[ "$IS_BROADCAST_NODE" ]]; then
      echo taiyi: beginning download and decompress of s3://$S3_BUCKET/broadcast-latest.tar.lz4
      aws s3 cp s3://$S3_BUCKET/broadcast-latest.tar.lz4 - | lz4 -d | tar x --wildcards 'blockchain/block*' -C /mnt/ramdisk 'blockchain/shared*'
    elif [[ "$IS_AH_NODE" ]]; then
      echo taiyi: beginning download and decompress of s3://$S3_BUCKET/ahnode-latest.tar.lz4
      aws s3 cp s3://$S3_BUCKET/ahnode-latest.tar.lz4 - | lz4 -d | tar x --wildcards 'blockchain/block*' 'blockchain/*rocksdb-storage*' -C /mnt/ramdisk 'blockchain/shared*'
    else
      echo taiyi: beginning download and decompress of s3://$S3_BUCKET/blockchain-latest.tar.lz4
      aws s3 cp s3://$S3_BUCKET/blockchain-latest.tar.lz4 - | lz4 -d | tar x --wildcards 'blockchain/block*' -C /mnt/ramdisk 'blockchain/shared*'
    fi
    if [[ $? -ne 0 ]]; then
      sleep 1
      echo notifyalert taiyi: unable to pull blockchain state from S3 - attempt $count
      (( count++ ))
    else
      finished=1
    fi
  done
  chown -R taiyi:taiyi /mnt/ramdisk/blockchain
else
  while [[ $count -le 5 ]] && [[ $finished == 0 ]]
  do
    rm -rf $HOME/blockchain/*
    if [[ "$IS_BROADCAST_NODE" ]]; then
      echo taiyi: beginning download and decompress of s3://$S3_BUCKET/broadcast-latest.tar.lz4
      aws s3 cp s3://$S3_BUCKET/broadcast-latest.tar.lz4 - | lz4 -d | tar x
    elif [[ "$IS_AH_NODE" ]]; then
      echo taiyi: beginning download and decompress of s3://$S3_BUCKET/ahnode-latest.tar.lz4
      aws s3 cp s3://$S3_BUCKET/ahnode-latest.tar.lz4 - | lz4 -d | tar x
    else
      echo taiyi: beginning download and decompress of s3://$S3_BUCKET/blockchain-latest.tar.lz4
      aws s3 cp s3://$S3_BUCKET/blockchain-latest.tar.lz4 - | lz4 -d | tar x
    fi
    if [[ $? -ne 0 ]]; then
      sleep 1
      echo notifyalert taiyi: unable to pull blockchain state from S3 - attempt $count
      (( count++ ))
    else
      finished=1
    fi
  done
fi

# remove download file tag
rm /tmp/isdownloading
echo "[info] remove /tmp/isdownloading."

if [[ $finished == 0 ]]; then
  if [[ ! "$SYNC_TO_S3" ]]; then
    echo notifyalert taiyi: unable to pull blockchain state from S3 - exiting
    exit 1
  else
    echo notifytaiyisync taiyisync: shared memory file for $VERSION not found, creating a new one by replaying the blockchain
    if [[ "$USE_RAMDISK" ]]; then
      mkdir -p /mnt/ramdisk/blockchain
      chown -R taiyi:taiyi /mnt/ramdisk/blockchain
    else
      mkdir blockchain
    fi

    # add a tag file to help check if download process has been done.
    echo "[info] create downloading tag file."
    touch /tmp/isdownloading
    aws s3 cp s3://$S3_BUCKET/block_log-latest blockchain/block_log
    # remove download file tag
    rm /tmp/isdownloading
    echo "[info] remove /tmp/isdownloading."

    if [[ $? -ne 0 ]]; then
      echo notifytaiyisync taiyisync: unable to pull latest block_log from S3, will sync from scratch.
    else
      ARGS+=" --replay-blockchain --force-validate"
    fi
    touch /tmp/isnewsync
  fi
fi

cd $HOME

chown -R taiyi:taiyi $HOME/*

# let's get going
exec chpst -utaiyi \
    $TAIYIN \
        --webserver-ws-endpoint=127.0.0.1:8091 \
        --webserver-http-endpoint=127.0.0.1:8091 \
        --p2p-endpoint=0.0.0.0:2025 \
        --data-dir=$HOME \
        $ARGS \
        $TAIYI_EXTRA_OPTS \
        2>&1&
#sed -i 's/ahnode.taiyi.com/127.0.0.1:8091/' /etc/nginx/healthcheck.conf
#service nginx restart
SAVED_PID=`pgrep -f p2p-endpoint`
echo $SAVED_PID >> /tmp/taiyinpid
mkdir -p /etc/service/taiyi
if [[ ! "$SYNC_TO_S3" ]]; then
  cp /usr/local/bin/paas-sv-run.sh /etc/service/taiyi/run
else
  cp /usr/local/bin/sync-sv-run.sh /etc/service/taiyi/run
fi
chmod +x /etc/service/taiyi/run
runsv /etc/service/taiyi
