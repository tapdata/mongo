#!/usr/bin/env python
#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.
#

import datetime, inspect, os, random, wiredtiger

# These routines help run the various storage sources. They are required to manage
# generation of storage source specific configurations.

# Generate a storage store specific authentication token.
def get_auth_token(storage_source):
    auth_token = None
    if storage_source == 'dir_store':
        # Fake a secret token.
        auth_token = "Secret"
    if storage_source == 's3_store':
        # Auth token is the AWS access key ID and the AWS secret key as semi-colon separated values.
        # We expect the values to have been provided through the environment variables.
        access_key = os.getenv('aws_sdk_s3_ext_access_key')
        secret_key = os.getenv('aws_sdk_s3_ext_secret_key')
        if access_key and secret_key:
            auth_token = access_key + ";" + secret_key
    return auth_token

# Get buckets configured for the storage source

# S3 buckets with their regions
s3_buckets = ['s3testext-us;us-east-2', 's3testext;ap-southeast-2']

# Local buckets do not have a region
local_buckets = ['bucket1', 'bucket2']

# Get name of the bucket at specified index in the list.
def get_bucket_name(storage_source, i):
    if storage_source == 's3_store':
        return s3_buckets[i]
    if storage_source == 'dir_store':
        return local_buckets[i]
    return None

# Set up configuration
def get_conn_config(storage_source):
    if not storage_source.is_tiered_scenario():
            return ''
    if storage_source.ss_name == 'dir_store' and not os.path.exists(storage_source.bucket):
            os.mkdir(storage_source.bucket)
    return \
        'statistics=(all),' + \
        'tiered_storage=(auth_token=%s,' % storage_source.auth_token + \
        'bucket=%s,' % storage_source.bucket + \
        'bucket_prefix=%s,' % storage_source.bucket_prefix + \
        'name=%s,' % storage_source.ss_name

def get_check(storage_source, tc, base, n):
    for i in range(base, n):
        storage_source.assertEqual(tc[str(i)], str(i))
    tc.set_key(str(n))
    storage_source.assertEquals(tc.search(), wiredtiger.WT_NOTFOUND)

# Generate a unique object prefix for the S3 store. 
def generate_s3_prefix(random_prefix = '', test_name = ''):
    # Generates a unique prefix to be used with the object keys, eg:
    # "s3test/python/2022-31-01-16-34-10/623843294--".
    # Objects with the prefix pattern "s3test/*" are deleted after a certain period of time 
    # according to the lifecycle rule on the S3 bucket. Should you wish to make any changes to the
    # prefix pattern or lifecycle of the object, please speak to the release manager.
    # Group all the python test objects under s3test/python/ 
    prefix = 's3test/python/'
    # Group each test run together by random number and date. If a random prefix isn't provided
    # generate a new one now.
    if random_prefix is None:
        random_prefix = str(random.randrange(1, 2147483646))
    prefix += datetime.datetime.now().strftime('%Y-%m-%d-%H-%M') + '--' + random_prefix +'/'
    # Group all scenarios from the same test under the same test name.
    prefix += test_name + '/'

    # Generate a random number to differentiate object files for tests that use multiple bucket
    # prefixes. It is important to generate unique prefixes for different tests in the same class,
    # so that the database namespace do not collide.
    # Range up to int32_max, matches that of C++'s std::default_random_engine
    prefix += str(random.randrange(1, 2147483646)) + '--'

    return prefix

def gen_tiered_storage_sources(random_prefix='', test_name='', tiered_only=False):
    tiered_storage_sources = [
        ('dirstore', dict(is_tiered = True,
            is_local_storage = True,
            auth_token = get_auth_token('dir_store'),
            bucket = get_bucket_name('dir_store', 0),
            bucket1 = get_bucket_name('dir_store', 1),
            bucket_prefix = "pfx_",
            bucket_prefix1 = "pfx1_",
            bucket_prefix2 = 'pfx2_',
            num_ops=100,
            ss_name = 'dir_store')),
        ('s3', dict(is_tiered = True,
            is_local_storage = False,
            auth_token = get_auth_token('s3_store'),
            bucket = get_bucket_name('s3_store', 0),
            bucket1 = get_bucket_name('s3_store', 1),
            bucket_prefix = generate_s3_prefix(random_prefix, test_name),
            bucket_prefix1 = generate_s3_prefix(random_prefix, test_name),
            bucket_prefix2 = generate_s3_prefix(random_prefix, test_name),
            num_ops=20,
            ss_name = 's3_store')),
        ('non_tiered', dict(is_tiered = False)),
    ]

    # Return a sublist to use for the tiered test scenarios as last item on list is not a scenario
    # for the tiered tests.  
    if tiered_only:
        return tiered_storage_sources[:2]

    return tiered_storage_sources

# This mixin class provides tiered storage configuration methods.
class TieredConfigMixin:
    # Returns True if the current scenario is tiered.
    def is_tiered_scenario(self):
        return hasattr(self, 'is_tiered') and self.is_tiered

    # Setup connection config.
    def conn_config(self):
        return self.tiered_conn_config()

    # Setup tiered connection config.
    def tiered_conn_config(self):
        # Handle non_tiered storage scenarios.
        if not self.is_tiered_scenario():
            return ''

        # Setup directories structure for local tiered storage.
        if self.is_local_storage:
            bucket_full_path = os.path.join(self.home, self.bucket)
            if not os.path.exists(bucket_full_path):
                os.mkdir(bucket_full_path)

        # Build tiered storage connection string.
        return \
            'tiered_storage=(auth_token=%s,' % self.auth_token + \
            'bucket=%s,' % self.bucket + \
            'bucket_prefix=%s,' % self.bucket_prefix + \
            'name=%s),' % self.ss_name

    # Load the storage sources extension.
    def conn_extensions(self, extlist):
        return self.tiered_conn_extensions(extlist)

    # Load tiered storage source extension.
    def tiered_conn_extensions(self, extlist):
        # Handle non_tiered storage scenarios.
        if not self.is_tiered_scenario():
            return ''
        
        config = ''
        # S3 store is built as an optional loadable extension, not all test environments build S3.
        if not self.is_local_storage:
            #config = '=(config=\"(verbose=1)\")'
            extlist.skip_if_missing = True
        #if self.is_local_storage:
            #config = '=(config=\"(verbose=1,delay_ms=200,force_delay=3)\")'
        # Windows doesn't support dynamically loaded extension libraries.
        if os.name == 'nt':
            extlist.skip_if_missing = True
        extlist.extension('storage_sources', self.ss_name + config)

    def download_objects(self, bucket_name, prefix):
        import boto3
        # The bucket from the storage source is expected to be a name and a region, separated by a 
        # semi-colon. eg: 'abcd;ap-southeast-2'.
        bucket_name, region = bucket_name.split(';')
        
        # Get the bucket resource and list the objects within that bucket that match the prefix for a
        # given test.
        s3 = boto3.resource('s3')
        bucket = s3.Bucket(bucket_name)
        objects = list(bucket.objects.filter(Prefix=prefix))

        # Create a directory within the test directory to download the objects to.
        s3_object_files_path = 's3_objects/'
        if not os.path.exists(s3_object_files_path):
            os.makedirs(s3_object_files_path)

        for o in objects:
            filename = s3_object_files_path + '/' + o.key.split('/')[-1]
            bucket.download_file(o.key, filename)
