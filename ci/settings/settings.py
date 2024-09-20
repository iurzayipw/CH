from ci.settings.definitions import RunnerLabels

S3_ARTIFACT_PATH = "clickhouse-builds/artifacts"
CI_CONFIG_RUNS_ON = [RunnerLabels.CI_SERVICES]
DOCKER_BUILD_RUNS_ON = [RunnerLabels.CI_SERVICES_EBS]
CACHE_S3_PATH = "clickhouse-builds/ci_ch_cache"
HTML_S3_PATH = "clickhouse-builds/artifacts/reports"
S3_BUCKET_TO_HTTP_ENDPOINT = {"clickhouse-builds": "clickhouse-builds.s3.amazonaws.com"}

DOCKERHUB_USERNAME = "robotclickhouse"
DOCKERHUB_SECRET = "dockerhub_robot_password"

CI_DB_DB_NAME = "default"
CI_DB_TABLE_NAME = "checks"


SECRET_GH_APP_ID: str = "woolenwolf_gh_app.clickhouse-app-id"
SECRET_GH_APP_PEM_KEY: str = "woolenwolf_gh_app.clickhouse-app-key"

INSTALL_PYTHON_REQS_FOR_NATIVE_JOBS = ""
