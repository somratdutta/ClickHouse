# Building Docker Images for Your ClickHouse Fork

This guide explains how to build and access Docker images from your ClickHouse fork using GitHub Actions.

## Setup

1. **Enable GitHub Container Registry**: The workflow is already configured to push to GitHub Container Registry (ghcr.io), which is free for public repositories.

2. **Permissions**: The workflow uses `GITHUB_TOKEN` which is automatically available. No additional secrets are needed.

3. **Trigger the Build**: The workflow will run automatically when you:
   - Push to `main`, `master`, or `develop` branches
   - Create a pull request
   - Create a tag starting with `v` (e.g., `v1.0.0`)
   - Manually trigger it from the Actions tab

## Images Built

The workflow builds two Docker images:

### ClickHouse Server
- **Image**: `ghcr.io/YOUR_USERNAME/clickhouse/clickhouse-server`
- **Based on**: Ubuntu 22.04
- **Platforms**: linux/amd64, linux/arm64

### ClickHouse Keeper
- **Image**: `ghcr.io/YOUR_USERNAME/clickhouse/clickhouse-keeper` 
- **Based on**: Alpine Linux
- **Platforms**: linux/amd64, linux/arm64

## Image Tags

Images are automatically tagged based on the trigger:

- `latest` - Latest build from default branch
- `main` - Latest build from main branch  
- `develop` - Latest build from develop branch
- `pr-123` - Pull request #123
- `v1.0.0` - Semantic version tags
- `main-abc1234` - Branch name + short commit SHA

## Accessing Your Images

### 1. Make Images Public (Recommended)

By default, images are private. To make them public:

1. Go to your repository on GitHub
2. Click on "Packages" tab
3. Click on your image package
4. Go to "Package settings"
5. Scroll down and click "Change visibility"
6. Select "Public"

### 2. Pull the Images

Once public, anyone can pull your images:

```bash
# Pull ClickHouse Server
docker pull ghcr.io/YOUR_USERNAME/clickhouse/clickhouse-server:latest

# Pull ClickHouse Keeper  
docker pull ghcr.io/YOUR_USERNAME/clickhouse/clickhouse-keeper:latest

# Pull specific version
docker pull ghcr.io/YOUR_USERNAME/clickhouse/clickhouse-server:v1.0.0
```

### 3. Run Your Custom ClickHouse

```bash
# Run ClickHouse Server
docker run -d \
  --name clickhouse-server \
  -p 8123:8123 \
  -p 9000:9000 \
  ghcr.io/YOUR_USERNAME/clickhouse/clickhouse-server:latest

# Run ClickHouse Keeper
docker run -d \
  --name clickhouse-keeper \
  -p 2181:2181 \
  -p 2888:2888 \
  -p 3888:3888 \
  ghcr.io/YOUR_USERNAME/clickhouse/clickhouse-keeper:latest
```

### 4. Using Docker Compose

Create a `docker-compose.yml`:

```yaml
version: '3.8'
services:
  clickhouse:
    image: ghcr.io/YOUR_USERNAME/clickhouse/clickhouse-server:latest
    ports:
      - "8123:8123"
      - "9000:9000"
    volumes:
      - clickhouse_data:/var/lib/clickhouse
    environment:
      CLICKHOUSE_DB: default
      CLICKHOUSE_USER: default
      CLICKHOUSE_DEFAULT_ACCESS_MANAGEMENT: 1

volumes:
  clickhouse_data:
```

## Private Access

If you keep images private, users need to authenticate:

```bash
# Login with GitHub token
echo $GITHUB_TOKEN | docker login ghcr.io -u YOUR_USERNAME --password-stdin

# Or login with personal access token
echo $PERSONAL_ACCESS_TOKEN | docker login ghcr.io -u YOUR_USERNAME --password-stdin
```

## Customizing the Build

### Build Arguments

You can modify the workflow to pass build arguments:

```yaml
- name: Build and push Docker image
  uses: docker/build-push-action@v5
  with:
    context: ./docker/server
    file: ./docker/server/Dockerfile.ubuntu
    build-args: |
      VERSION=25.8.2.29
      REPO_CHANNEL=stable
    # ... other parameters
```

### Custom Dockerfiles

Create your own Dockerfiles in your fork and update the workflow:

```yaml
file: ./docker/server/Dockerfile.custom
```

### Build Matrix

Add multiple variants:

```yaml
strategy:
  matrix:
    variant: [ubuntu, alpine]
    
# Use in build step:
file: ./docker/server/Dockerfile.${{ matrix.variant }}
```

## Monitoring Builds

1. Go to your repository's "Actions" tab
2. Click on "Build Docker Images (Fork)" workflow
3. View logs and download artifacts
4. Check the "Packages" tab to see published images

## Troubleshooting

### Build Fails
- Check the Actions logs for detailed error messages
- Ensure Dockerfiles exist in the specified paths
- Verify build context has necessary files

### Permission Issues
- Ensure the repository has Actions enabled
- Check that `GITHUB_TOKEN` has package write permissions
- Verify Container Registry is enabled for your account

### Image Not Found
- Check if the image was built successfully in Actions
- Verify the image name and tag
- Ensure the package is public or you're authenticated

## Next Steps

- Consider setting up automated testing before image builds
- Add security scanning with tools like Trivy
- Set up image signing for production use
- Configure retention policies for old images

Replace `YOUR_USERNAME` with your actual GitHub username in all examples above.
